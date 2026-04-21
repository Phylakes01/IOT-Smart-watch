#include <WiFi.h>
#include <time.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "MAX30105.h"
#include "heartRate.h"
#include <math.h>

// ===================== OLED =====================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
#define OLED_SDA 21
#define OLED_SCL 22
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ===================== MAX30102 =====================
MAX30105 particleSensor;

// ===================== WIFI / NTP =====================
const char* WIFI_SSID = "Alexer";
const char* WIFI_PASS = "1234567890";
const char* TZ_INFO   = "PST-8";

volatile bool timeReady = false;
volatile bool ntpStarted = false;

// ===== FIXED WIFI STATE MACHINE =====
unsigned long wifiAttemptStartMs = 0;
unsigned long lastWifiActionMs   = 0;
bool wifiBeginCalled = false;

const unsigned long WIFI_RETRY_INTERVAL_MS  = 1000;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 12000;

unsigned long lastNtpKickMs = 0;
const unsigned long NTP_KICK_MS = 1200;

bool wifiConnected() { return WiFi.status() == WL_CONNECTED; }

void wifiBeginOnce() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  wifiAttemptStartMs = millis();
  wifiBeginCalled = true;
}

void wifiService() {
  if (!wifiBeginCalled) {
    wifiBeginOnce();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) return;

  if (millis() - lastWifiActionMs < WIFI_RETRY_INTERVAL_MS) return;
  lastWifiActionMs = millis();

  if (millis() - wifiAttemptStartMs > WIFI_CONNECT_TIMEOUT_MS) {
    WiFi.disconnect();
    delay(50);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    wifiAttemptStartMs = millis();
  }
}

void startWiFi() {
  wifiBeginCalled = false;
  wifiBeginOnce();
}

bool tryUpdateTime(unsigned long timeoutMs = 10) {
  struct tm t;
  if (getLocalTime(&t, timeoutMs)) {
    timeReady = true;
    return true;
  }
  return false;
}

void startNtpOnce() {
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  ntpStarted = true;
}

// ===================== TELEGRAM =====================
const char* BOT_TOKEN = "7950607842:AAE5sAQK5m41bgRjZ9j2n-PPa5QHa1dKbQc";
const char* CHAT_ID   = "5222698834";

WiFiClientSecure tgClient;
UniversalTelegramBot bot(BOT_TOKEN, tgClient);

unsigned long lastBotPollMs = 0;
const unsigned long BOT_POLL_MS = 1200;

const unsigned long ALERT_COOLDOWN_MS = 60000;
unsigned long lastBpmAlertMs  = 0;
unsigned long lastTempAlertMs = 0;

bool lastWiFiState = false;

// ===================== TIME HELPERS =====================
String getDateTimeString() {
  if (!timeReady) return String("(syncing)");
  struct tm t;
  if (!getLocalTime(&t, 20)) return String("(syncing)");

  int hour12 = t.tm_hour % 12;
  if (hour12 == 0) hour12 = 12;
  const char* ap = (t.tm_hour >= 12) ? "PM" : "AM";

  char buf[28];
  snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d:%02d %s",
           t.tm_mon + 1, t.tm_mday, hour12, t.tm_min, t.tm_sec, ap);
  return String(buf);
}

String getTimeOnly12h() {
  if (!timeReady) return String("--:-- --");
  struct tm t;
  if (!getLocalTime(&t, 10)) return String("--:-- --");
  int hour12 = t.tm_hour % 12;
  if (hour12 == 0) hour12 = 12;
  const char* ap = (t.tm_hour >= 12) ? "PM" : "AM";
  char buf[12];
  snprintf(buf, sizeof(buf), "%02d:%02d %s", hour12, t.tm_min, ap);
  return String(buf);
}

String getDateOnly() {
  if (!timeReady) return String("--/--");
  struct tm t;
  if (!getLocalTime(&t, 10)) return String("--/--");
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d/%02d", t.tm_mon + 1, t.tm_mday);
  return String(buf);
}

// ===================== SHARED VITALS =====================
volatile bool fingerPresent = false;
volatile int  beatAvg = 0;
volatile unsigned long lastBeatMsShared = 0;

volatile bool tempValid = false;
volatile float bodyC = NAN;
volatile unsigned long lastTempValidMs = 0;

// ===================== THERMISTOR =====================
#define THERM_PIN 35
const float SERIES_RESISTOR = 10000.0;
const float THERM_NOMINAL   = 10000.0;
const float TEMP_NOMINAL_C  = 25.0;
const float B_COEFF         = 3950.0;
const float TEMP_OFFSET     = 0.0;

unsigned long lastTempMs = 0;
const unsigned long TEMP_INTERVAL_MS = 350;
const float TEMP_ALPHA = 0.12f;
const float HIGH_TEMP_C = 38.0f;

bool readBodyTempRaw(float &outC) {
  const int SAMPLES = 18;
  long mvSum = 0;
  for (int i = 0; i < SAMPLES; i++) {
    mvSum += analogReadMilliVolts(THERM_PIN);
    delay(2);
  }

  float mv = mvSum / (float)SAMPLES;
  float v  = mv / 1000.0f;
  const float VCC = 3.3f;
  if (v < 0.02f || v > (VCC - 0.02f)) return false;

  float rA = SERIES_RESISTOR * (v / (VCC - v));
  float rB = SERIES_RESISTOR * ((VCC - v) / v);

  auto score = [](float r) {
    if (r < 500.0f || r > 500000.0f) return 999999.0f;
    return fabs(r - 10000.0f);
  };

  float rTherm = (score(rA) <= score(rB)) ? rA : rB;
  if (rTherm < 200.0f || rTherm > 500000.0f) return false;

  float invT = (1.0f / (TEMP_NOMINAL_C + 273.15f)) + (log(rTherm / THERM_NOMINAL) / B_COEFF);
  float T = 1.0f / invT;
  outC = (T - 273.15f) + TEMP_OFFSET;

  if (outC < -10.0f || outC > 120.0f) return false;
  return true;
}

void updateBodyTempI2CTask() {
  unsigned long now = millis();
  if (now - lastTempMs < TEMP_INTERVAL_MS) return;
  lastTempMs = now;

  float tC;
  bool ok = readBodyTempRaw(tC);
  tempValid = ok;
  if (!ok) return;

  float cur = bodyC;
  if (isnan(cur)) cur = tC;
  cur = TEMP_ALPHA * tC + (1.0f - TEMP_ALPHA) * cur;
  bodyC = cur;
  lastTempValidMs = now;
}

// ===================== BUZZER =====================
#define BUZZER_PIN 25
const int BUZZER_RES = 8;
const int HIGH_BPM = 120;

void buzzerInit() {
  ledcAttach(BUZZER_PIN, 4000, BUZZER_RES);
  ledcWrite(BUZZER_PIN, 0);
}
inline void buzzerToneDuty(int freq, int duty) {
  duty = constrain(duty, 0, 255);
  ledcWriteTone(BUZZER_PIN, freq);
  ledcWrite(BUZZER_PIN, duty);
}
inline void buzzerOff() { ledcWrite(BUZZER_PIN, 0); }

unsigned long beatBeepUntilMs = 0;
void beepOnBeatNB() {
  buzzerToneDuty(2200, 170);
  beatBeepUntilMs = millis() + 35;
}

unsigned long alertTickMs = 0;
bool alertOn = false;
void alertService(bool highBpm, bool highTemp) {
  if (!(highBpm || highTemp)) {
    alertOn = false;
    return;
  }
  if (millis() - alertTickMs >= 150) {
    alertTickMs = millis();
    alertOn = !alertOn;
    if (alertOn) {
      int f = 2500;
      if (highBpm) f = 3000;
      if (highBpm && highTemp) f = 3300;
      buzzerToneDuty(f, 230);
    } else {
      buzzerOff();
    }
  }
}
void beepService() {
  if (beatBeepUntilMs && millis() > beatBeepUntilMs) {
    buzzerOff();
    beatBeepUntilMs = 0;
  }
}

// ===================== WIFI ICON =====================
int wifiBarsFromRSSI(int rssi) {
  if (rssi >= -55) return 3;
  if (rssi >= -67) return 2;
  if (rssi >= -80) return 1;
  return 0;
}
void drawWifiIcon(int x, int y) {
  if (!wifiConnected()) {
    display.drawLine(x, y, x + 7, y + 7, SSD1306_WHITE);
    display.drawLine(x + 7, y, x, y + 7, SSD1306_WHITE);
    return;
  }
  int bars = wifiBarsFromRSSI(WiFi.RSSI());
  int bx = x;
  for (int i = 0; i < 4; i++) {
    int h = (i + 1) * 2;
    int top = (y + 7) - h + 1;
    if (i <= bars) display.fillRect(bx, top, 2, h, SSD1306_WHITE);
    else display.drawRect(bx, top, 2, h, SSD1306_WHITE);
    bx += 3;
  }
}

// ===================== HEART ICON =====================
const unsigned char heart16[] PROGMEM = {
  0b00001100,0b00110000,
  0b00011110,0b01111000,
  0b00111111,0b11111100,
  0b01111111,0b11111110,
  0b11111111,0b11111111,
  0b11111111,0b11111111,
  0b11111111,0b11111111,
  0b11111111,0b11111111,
  0b01111111,0b11111110,
  0b00111111,0b11111100,
  0b00011111,0b11111000,
  0b00001111,0b11110000,
  0b00000111,0b11100000,
  0b00000011,0b11000000,
  0b00000001,0b10000000,
  0b00000000,0b00000000
};

// ===================== GRAPH =====================
static const int GRAPH_Y = 54;
static const int GRAPH_H = 10;
uint8_t graphVals[SCREEN_WIDTH];

void graphClear() {
  for (int i = 0; i < SCREEN_WIDTH; i++) graphVals[i] = 255;
}
void graphPush(uint8_t v) {
  for (int i = 0; i < SCREEN_WIDTH - 1; i++) graphVals[i] = graphVals[i + 1];
  graphVals[SCREEN_WIDTH - 1] = v;
}
void drawGraphLine() {
  for (int x = 1; x < SCREEN_WIDTH; x++) {
    if (graphVals[x - 1] == 255 || graphVals[x] == 255) continue;
    int y0 = (GRAPH_Y + GRAPH_H - 1) - graphVals[x - 1];
    int y1 = (GRAPH_Y + GRAPH_H - 1) - graphVals[x];
    display.drawLine(x - 1, y0, x, y1, SSD1306_WHITE);
  }
}
uint8_t pulseWave(unsigned long now, int bpm, unsigned long lastBeatMs) {
  if (bpm <= 0 || lastBeatMs == 0) return 0;
  float period = 60000.0f / (float)bpm;
  float phase  = fmod((now - lastBeatMs) / period, 1.0f);

  const float RISE = 0.10f;
  const float FALL = 0.30f;

  float amp = 0.0f;
  if (phase < RISE) amp = phase / RISE;
  else if (phase < (RISE + FALL)) {
    float t = (phase - RISE) / FALL;
    amp = 1.0f - t;
    amp = amp * amp;
  } else amp = 0.0f;

  amp = constrain(amp, 0.0f, 1.0f);
  return (uint8_t)(amp * (GRAPH_H - 2));
}

// ===================== FAST PULSE SETTINGS =====================
const byte RATE_SIZE = 2;   // mas mabilis update
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;

uint8_t irPower  = 0xA0;
uint8_t redPower = 0x18;    // visible red LED ON

const uint8_t IR_POWER_MIN  = 0x45;
const uint8_t RED_POWER_MIN = 0x08;
const uint8_t RED_POWER_MAX = 0x30;

const long IR_CLIP    = 260000;
const long IR_CLIP_HI = 240000;

const long IR_TARGET = 100000;
const long IR_LOW    = 22000;
const long IR_HIGH   = 190000;

const long CONTACT_IR_MIN = 15000;  // mas mabilis contact detect

unsigned long lastNoContactMs   = 0;
unsigned long contactStartMs    = 0;
unsigned long lastGoodBeatMs    = 0;
volatile uint8_t presentScore   = 0;

// ===================== CORE 1 TASK =====================
void i2cSensorDisplayTask(void* pv) {
  unsigned long lastLedTuneMs = 0;
  const unsigned long DISP_INTERVAL_MS = 25;
  unsigned long lastDispMs = 0;

  while (true) {
    unsigned long now = millis();

    long irValue = particleSensor.getIR();
    bool clipped = (irValue >= IR_CLIP);

    bool contact = (!clipped) && (irValue >= CONTACT_IR_MIN);

    if (contact) {
      if (presentScore < 10) presentScore++;
      if (contactStartMs == 0) contactStartMs = now;
    } else {
      if (presentScore > 0) presentScore--;
      contactStartMs = 0;
      lastNoContactMs = now;
    }

    fingerPresent = (presentScore >= 1);
    bool contactStable = (contactStartMs != 0) && (now - contactStartMs > 350);

    // adaptive LED power
    if (now - lastLedTuneMs > 100) {
      lastLedTuneMs = now;

      if (irValue >= IR_CLIP_HI) {
        irPower = (uint8_t)max((int)IR_POWER_MIN, (int)irPower - 0x18);
      } else if (contact) {
        if (irValue < IR_LOW) irPower = (uint8_t)min(255, (int)irPower + 0x12);
        else if (irValue > IR_HIGH) irPower = (uint8_t)max((int)IR_POWER_MIN, (int)irPower - 0x12);
        else {
          if (irValue < IR_TARGET) irPower = (uint8_t)min(255, (int)irPower + 0x06);
          else                     irPower = (uint8_t)max((int)IR_POWER_MIN, (int)irPower - 0x06);
        }
      } else {
        irPower = (uint8_t)max((int)IR_POWER_MIN, (int)irPower - 0x06);
      }

      if (contact) redPower = constrain((int)(irPower / 8), RED_POWER_MIN, RED_POWER_MAX);
      else redPower = RED_POWER_MIN;

      particleSensor.setPulseAmplitudeIR(irPower);
      particleSensor.setPulseAmplitudeRed(redPower);
    }

    updateBodyTempI2CTask();

    // reset when finger removed
    if (!contact && (now - lastNoContactMs > 400)) {
      beatAvg = 0;
      lastBeat = 0;
      lastBeatMsShared = 0;
      lastGoodBeatMs = 0;
      for (int i = 0; i < RATE_SIZE; i++) rates[i] = 0;
      rateSpot = 0;
      graphClear();
    }

    // reset when reading is stuck
    if (fingerPresent && lastGoodBeatMs != 0 && (now - lastGoodBeatMs > 2500)) {
      beatAvg = 0;
      lastBeatMsShared = 0;
      for (int i = 0; i < RATE_SIZE; i++) rates[i] = 0;
      rateSpot = 0;
    }

    // pulse detection
    if (contactStable) {
      if (checkForBeat(irValue)) {
        long delta = millis() - lastBeat;
        lastBeat = millis();
        lastBeatMsShared = (unsigned long)lastBeat;

        if (delta > 0) {
          float bpm = 60.0f / (delta / 1000.0f);

          if (bpm > 40 && bpm < 220) {
            rates[rateSpot++] = (byte)bpm;
            rateSpot %= RATE_SIZE;

            int sum = 0;
            int validCount = 0;
            for (byte x = 0; x < RATE_SIZE; x++) {
              if (rates[x] > 0) {
                sum += rates[x];
                validCount++;
              }
            }

            if (validCount > 0) {
              beatAvg = sum / validCount;
              lastGoodBeatMs = now;
            }

            beepOnBeatNB();
          }
        }
      }
    }

    bool highBpm  = (fingerPresent && beatAvg >= HIGH_BPM && beatAvg > 0);
    bool highTemp = (tempValid && !isnan((float)bodyC) && ((float)bodyC) >= HIGH_TEMP_C);
    alertService(highBpm, highTemp);
    beepService();

    if (now - lastDispMs > DISP_INTERVAL_MS) {
      lastDispMs = now;

      display.clearDisplay();

      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 0);
      display.print(getDateOnly());
      display.print(" ");
      display.print(getTimeOnly12h());
      drawWifiIcon(112, 0);

      display.setCursor(0, 14);
      display.setTextSize(1);
      display.print("BPM:");

      display.setTextSize(2);
      display.setCursor(40, 10);
      if (fingerPresent && beatAvg > 0) display.print(beatAvg);
      else display.print("--");

      display.drawBitmap(0, 28, heart16, 16, 16, SSD1306_WHITE);

      display.setTextSize(1);
      display.setCursor(22, 30);
      display.print("Temp:");

      display.setTextSize(2);
      display.setCursor(60, 26);
      if (tempValid && !isnan((float)bodyC)) {
        display.print((float)bodyC, 1);
        display.print("C");
      } else {
        display.print("--.-C");
      }

      uint8_t v = (fingerPresent && beatAvg > 0) ? pulseWave(now, beatAvg, lastBeatMsShared) : 255;
      graphPush(v);
      drawGraphLine();

      display.display();
    }

    static unsigned long lastDbg = 0;
    if (now - lastDbg > 500) {
      lastDbg = now;
      Serial.print("IR="); Serial.print(irValue);
      Serial.print(" pwrIR="); Serial.print(irPower);
      Serial.print(" pwrRED="); Serial.print(redPower);
      Serial.print(" score="); Serial.print(presentScore);
      Serial.print(" stable="); Serial.print((int)contactStable);
      Serial.print(" bpm="); Serial.println(beatAvg);
    }

    vTaskDelay(1);
  }
}

// ===================== TELEGRAM =====================
String normalizeCmd(String s) {
  s.trim();
  s.toLowerCase();
  if (s.startsWith("/")) s.remove(0, 1);
  int at = s.indexOf('@');
  if (at >= 0) s = s.substring(0, at);
  return s;
}

String vitalsText(bool incPulse, bool incTemp, bool incStatus) {
  String msg;
  msg += "⌚ *IoT Smart Watch*\n";
  msg += "🕒 *Time:* " + getDateTimeString() + "\n";

  if (incStatus) {
    msg += "📶 *WiFi:* ";
    msg += wifiConnected() ? "Connected" : "Disconnected";
    if (wifiConnected()) msg += " (" + String(WiFi.RSSI()) + " dBm)";
    msg += "\n";
  }

  if (incPulse) {
    msg += "❤️ *Pulse:* ";
    if (!fingerPresent) msg += "--\n";
    else if (beatAvg <= 0) msg += "..\n";
    else msg += String(beatAvg) + " BPM\n";
  }

  if (incTemp) {
    msg += "🌡️ *Temp:* ";
    if (!tempValid || isnan((float)bodyC)) msg += "--\n";
    else msg += String((float)bodyC, 1) + " °C\n";
  }

  return msg;
}

void handleTelegramOnce() {
  if (!wifiConnected()) return;

  int numNew = bot.getUpdates(bot.last_message_received + 1);
  if (!numNew) return;

  for (int i = 0; i < numNew; i++) {
    if (bot.messages[i].chat_id != String(CHAT_ID)) continue;
    String cmd = normalizeCmd(bot.messages[i].text);

    if (cmd == "start") {
      bot.sendMessage(CHAT_ID, "✅ *IoT Smart Watch*\n/use /help", "Markdown");
    } else if (cmd == "help") {
      bot.sendMessage(CHAT_ID, "📌 *Commands*\n/start\n/help\n/status\n/pulse\n/temp\n/vitals\n", "Markdown");
    } else if (cmd == "status") {
      bot.sendMessage(CHAT_ID, vitalsText(false, false, true), "Markdown");
    } else if (cmd == "pulse") {
      bot.sendMessage(CHAT_ID, vitalsText(true, false, false), "Markdown");
    } else if (cmd == "temp") {
      bot.sendMessage(CHAT_ID, vitalsText(false, true, false), "Markdown");
    } else if (cmd == "vitals") {
      bot.sendMessage(CHAT_ID, vitalsText(true, true, false), "Markdown");
    } else {
      bot.sendMessage(CHAT_ID, "❓ Unknown command. Type */help*.", "Markdown");
    }
  }
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);

  Wire.begin(OLED_SDA, OLED_SCL);

  display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  display.setTextWrap(false);
  display.clearDisplay();
  display.display();

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  buzzerInit();
  graphClear();

  startWiFi();

  tgClient.setInsecure();
  tgClient.setTimeout(250);

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30102 not found");
    while (1) delay(100);
  }

  // mas mabilis kaysa dati
  // brightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange
  particleSensor.setup(0xA0, 4, 2, 400, 215, 16384);

  particleSensor.setPulseAmplitudeIR(irPower);
  particleSensor.setPulseAmplitudeRed(redPower);

  xTaskCreatePinnedToCore(
    i2cSensorDisplayTask,
    "i2cSensorDisplayTask",
    12000,
    nullptr,
    2,
    nullptr,
    1
  );
}

// ===================== LOOP =====================
void loop() {
  unsigned long now = millis();

  wifiService();

  if (!wifiConnected()) {
    timeReady = false;
    ntpStarted = false;
  } else {
    if (!ntpStarted) {
      startNtpOnce();
      lastNtpKickMs = now;
    }
    if (!timeReady && now - lastNtpKickMs > NTP_KICK_MS) {
      lastNtpKickMs = now;
      tryUpdateTime(8);
    }
  }

  bool w = wifiConnected();
  if (w && !lastWiFiState) bot.sendMessage(CHAT_ID, "⌚ IOT Smart Watch Connected", "");
  lastWiFiState = w;

  if (wifiConnected() && (now - lastBotPollMs > BOT_POLL_MS)) {
    lastBotPollMs = now;
    handleTelegramOnce();
  }

  bool highBpm  = (fingerPresent && beatAvg >= HIGH_BPM && beatAvg > 0);
  bool highTemp = (tempValid && !isnan((float)bodyC) && ((float)bodyC) >= HIGH_TEMP_C);

  if (wifiConnected()) {
    if (highBpm && (now - lastBpmAlertMs > ALERT_COOLDOWN_MS)) {
      String msg = "🚨 ALERT\n";
      msg += "❤️ Pulse: " + String(beatAvg) + " BPM\n";
      msg += "🌡️ Temp: " + (tempValid && !isnan((float)bodyC) ? String((float)bodyC, 1) + " °C\n" : String("--\n"));
      msg += "🕒 Time: " + getDateTimeString();
      bot.sendMessage(CHAT_ID, msg, "");
      lastBpmAlertMs = now;
    }

    if (highTemp && (now - lastTempAlertMs > ALERT_COOLDOWN_MS)) {
      String msg = "🚨 ALERT\n";
      msg += "🥤 Drink water Temperature is High\n";
      msg += "🌡️ Temp: " + String((float)bodyC, 1) + " °C\n";
      msg += "❤️ Pulse: " + (fingerPresent && beatAvg > 0 ? String(beatAvg) + " BPM\n" : String("--\n"));
      msg += "🕒 Time: " + getDateTimeString();
      bot.sendMessage(CHAT_ID, msg, "");
      lastTempAlertMs = now;
    }
  }

  delay(2);
}