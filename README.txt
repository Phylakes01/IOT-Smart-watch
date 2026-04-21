README.txt
ESP32 Smart Watch (MAX30102 + OLED + Thermistor + Telegram Bot)
Grade 12 SHS-Friendly Code Explanation
==============================================================

AUTHOR / OWNER:
- (Fill in your name here)

PROJECT SUMMARY
---------------
This program turns an ESP32 into a simple “smart watch” that can:
1) Connect to WiFi (auto reconnect).
2) Sync time using NTP (internet time).
3) Read heart rate (BPM) using MAX30102 (IR sensor).
4) Show BPM + temperature + time + graph on an OLED screen (SSD1306).
5) Send data and alerts to Telegram using a bot (/pulse, /temp, /vitals).
6) Sound a buzzer (beep on heartbeat + alarm pattern if high BPM/temp).

It uses TWO CPU cores of ESP32:
- Core 1: Sensor reading (I2C), BPM processing, OLED drawing, buzzer pattern
- Core 0: WiFi, NTP time sync, Telegram messages + polling

This makes the system fast and prevents “freezing” when WiFi/Telegram is slow.

--------------------------------------------------------------
HARDWARE NEEDED
---------------
1) ESP32 DevKit (any ESP32 board)
2) MAX30102 / MAX30105 heart rate sensor module
3) OLED SSD1306 128x64 I2C
4) Thermistor (NTC) + resistor divider (uses GPIO35 ADC)
5) Buzzer (active or passive; code uses LEDC tone output)
6) Wires + breadboard / PCB

--------------------------------------------------------------
PIN CONNECTIONS (as used in code)
---------------------------------
OLED (I2C):
- SDA = GPIO 21
- SCL = GPIO 22
- VCC = 3.3V
- GND = GND

MAX30102 (I2C, same bus as OLED):
- SDA = GPIO 21
- SCL = GPIO 22
- VCC = 3.3V (recommended)
- GND = GND

Thermistor input:
- THERM_PIN = GPIO 35 (ADC input)
- Uses analogReadMilliVolts() to measure voltage.
NOTE: You must build a voltage divider (thermistor + resistor) connected to 3.3V and GND.
Example (common):
- 3.3V -> 10k resistor -> junction -> thermistor -> GND
- Junction goes to GPIO35
(If you wire it the opposite way, it can still work because code tries both formulas,
but best to follow the common divider wiring above.)

Buzzer:
- BUZZER_PIN = GPIO 25
- GND to GND
- If passive buzzer: needs PWM/tone (this code supports that)
- If active buzzer: it will still beep but tone may behave differently

--------------------------------------------------------------
LIBRARIES YOU MUST INSTALL (Arduino IDE)
----------------------------------------
1) WiFi (built-in for ESP32)
2) time.h (built-in)
3) WiFiClientSecure (built-in for ESP32)
4) UniversalTelegramBot (install from Library Manager)
5) Adafruit GFX Library (Library Manager)
6) Adafruit SSD1306 (Library Manager)
7) SparkFun MAX3010x Pulse and Proximity Sensor Library (Library Manager)
   - provides MAX30105.h and heartRate.h

--------------------------------------------------------------
IMPORTANT SETTINGS YOU MUST EDIT
--------------------------------
WiFi:
const char* WIFI_SSID = "Tt";
const char* WIFI_PASS = "Jeremiah 2004";

Telegram:
const char* BOT_TOKEN = "YOUR_BOT_TOKEN";
const char* CHAT_ID   = "YOUR_CHAT_ID";

Time zone:
const char* TZ_INFO = "PST-8";   // UTC+8 (Philippines)

Alert thresholds:
const int HIGH_BPM = 120;        // BPM alert if >= 120
const float HIGH_TEMP_C = 38.0;  // Temp alert if >= 38C

--------------------------------------------------------------
CODE WALKTHROUGH (SECTION BY SECTION)
-------------------------------------

A) INCLUDE FILES
----------------
WiFi + time + HTTPS + Telegram + I2C + OLED + MAX30102 libraries.

B) OLED SETUP
-------------
- SCREEN_WIDTH/HEIGHT defines 128x64 OLED.
- OLED_ADDR is usually 0x3C.
- display is the OLED object used to draw text and graphics.

C) MAX30102 SETUP
-----------------
MAX30105 particleSensor;
Even though the board is MAX30102, the SparkFun library uses MAX30105 class.

D) WiFi / NTP VARIABLES
-----------------------
- WIFI_SSID/WIFI_PASS: your router credentials.
- TZ_INFO: timezone; "PST-8" means UTC+8.
- timeReady: becomes true once the ESP32 gets internet time.
- ntpStarted: true once configTzTime() is called.

E) FIXED WiFi STATE MACHINE (IMPORTANT)
---------------------------------------
This is the main “WiFi fix”. It avoids blocking delays and keeps reconnecting.

Key idea:
- startWiFi() calls WiFi.begin() once.
- wifiService() runs repeatedly in loop() to check:
  1) If connected: do nothing.
  2) If not connected:
     - Every 1 second, it checks if the connection attempt timed out.
     - If timed out (12 seconds), it restarts WiFi.begin().

Important fix:
- Uses WiFi.disconnect() NOT WiFi.disconnect(true).
  disconnect(true) can wipe settings and sometimes causes issues.
- WiFi.setSleep(false) makes WiFi more stable (less dropout).

F) TELEGRAM SETUP
-----------------
WiFiClientSecure tgClient;
UniversalTelegramBot bot(BOT_TOKEN, tgClient);

Important fix:
tgClient.setInsecure();
Telegram uses HTTPS. Without this, ESP32 may fail SSL connection.

G) TEMPERATURE (THERMISTOR)
---------------------------
- Reads GPIO35 ADC in millivolts.
- Converts voltage -> resistance -> temperature using Beta formula.
- TEMP_ALPHA smooths readings so they don’t jump.

H) BUZZER
---------
- Uses LEDC PWM hardware to generate tones.
- Beep on heartbeat (short beep).
- Alert beeps when high BPM or high temperature.

I) HEART ICON + PULSE GRAPH
---------------------------
- A heart icon is drawn using a bitmap array.
- A simple pulse “wave” animation is drawn at the bottom.

J) HEART RATE (BPM) LOGIC
-------------------------
- Reads IR value from MAX30102.
- Uses checkForBeat(irValue) to detect each beat.
- BPM is calculated from time between beats.
- Averages recent BPM values for stability.

Wearable/wrist improvements:
- CONTACT_IR_MIN and presentScore help detect real skin contact.
- irPower auto-adjusts IR LED brightness for better reading.

K) CORE 1 TASK (FAST SENSOR + OLED LOOP)
----------------------------------------
i2cSensorDisplayTask() runs on Core 1:
1) Read MAX30102 IR
2) Detect contact & adjust IR LED power
3) Compute BPM average
4) Read temperature
5) Update OLED
6) Run buzzer services

L) TELEGRAM COMMANDS
--------------------
/start  -> welcome
/help   -> list commands
/status -> WiFi + time
/pulse  -> BPM only
/temp   -> temperature only
/vitals -> BPM + temp

Only replies to the correct CHAT_ID for safety.

M) setup()
----------
- Starts Serial, I2C, OLED
- Sets ADC, buzzer
- Starts WiFi
- Enables Telegram HTTPS (setInsecure)
- Configures MAX30102
- Starts the Core 1 task

N) loop() (CORE 0)
------------------
- wifiService(): keeps WiFi connected (auto reconnect)
- NTP: sync time when connected
- Telegram polling: checks new commands regularly
- Sends alerts with cooldown (to prevent spam)
- Small delay(2) to reduce CPU load

--------------------------------------------------------------
HOW TO USE (STEPS)
------------------
1) Open Arduino IDE.
2) Install required libraries.
3) Select Board: "ESP32 Dev Module" (or your ESP32 board).
4) Select correct COM port.
5) Edit WIFI_SSID / WIFI_PASS.
6) Edit BOT_TOKEN / CHAT_ID.
7) Upload code.
8) Open Serial Monitor at 115200 baud.
9) Message your bot in Telegram: /start then /vitals

--------------------------------------------------------------
TROUBLESHOOTING
---------------
WiFi not connecting:
- Make sure router is 2.4GHz (ESP32 cannot use 5GHz).
- Check SSID spelling and CAPITAL letters (case-sensitive).
- Bring ESP32 closer to router.
- Try hotspot from phone to test.
- If still failing, print WiFi.status() in Serial Monitor.

Telegram not working:
- Make sure BOT_TOKEN and CHAT_ID are correct.
- Ensure tgClient.setInsecure() is present (it is in this code).
- If using a group chat, CHAT_ID usually starts with -100...

No BPM reading:
- Ensure MAX30102 wiring is correct and powered by 3.3V.
- Sensor needs stable skin contact for ~1 second.
- Thick casing/cover may block IR. Increase starting irPower if needed.

Temperature inaccurate:
- Thermistor model might have different B_COEFF.
- Calibrate TEMP_OFFSET (example: +1.0 or -1.0).

--------------------------------------------------------------
SAFETY NOTE
-----------
This is for learning / school demo only.
It is NOT a medical device. Readings may be inaccurate.

--------------------------------------------------------------
END OF FILE
