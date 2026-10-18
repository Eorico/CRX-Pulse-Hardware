/**
 * =====================================================================
 *  CRX-Pulse — PRODUCTION FIRMWARE (crx_pulse.ino)
 * =====================================================================
 *
 *  Built from the calibrated bench sketch (crx_pulse_test.ino — same
 *  board, same pins, same calibration constants) and extended with the
 *  production cloud layer. This is the thesis firmware, not a test
 *  sketch.
 *
 *  ARCHITECTURE (this build):
 *
 *      ESP32  ->  WiFi router  ->  Supabase (Postgres + PostgREST)
 *                              ->  Mobile app
 *
 *  No server code to deploy. The ESP32 inserts/reads rows directly over
 *  Supabase's auto REST API (PostgREST), sending the project's anon key
 *  in the `apikey` + `Authorization: Bearer` headers. Row Level Security
 *  policies (created once in the SQL editor) bound what the anon key can
 *  do. The mobile app reads the same tables (with Realtime) and writes
 *  feeding schedules / commands.
 *
 *  ---------------------------------------------------------------
 *  PARITY WITH THE BENCH SKETCH (crx_pulse_test.ino)
 *  ---------------------------------------------------------------
 *  Every sensor formula and calibration constant below is byte-for-byte
 *  the bench sketch's. Where this firmware HAD drifted from the bench
 *  logic, it has been brought back in line:
 *
 *   1. LAST-GOOD SENSOR VALUES. The bench sketch only ever overwrites
 *      airTemperature / humidity when the DHT11 read succeeds, so a
 *      single failed read (very common on a DHT11) is invisible — it
 *      keeps showing the last good value. This firmware now does the
 *      same via a GRACE WINDOW: a sensor stays "valid" for
 *      SENSOR_GRACE_MS after its last good read. Before, one dropped
 *      read flipped ambient_valid=false, which blanked Housing
 *      Temperature in the app AND cut the cooling fan mid-ramp.
 *
 *   2. COOLING FAN. Same ramp as the bench sketch (OFF < 25 C,
 *      191->255 across 25-40 C, full above 40 C), now driven by the
 *      last-good housing temperature instead of dropping to OFF on a
 *      single dropped DHT11 read.
 *
 *   3. AMMONIA ESTIMATE. The bench sketch interpolates ONLY inside the
 *      Sera reference band (V0..V2) and refuses to report outside it.
 *      This firmware previously extrapolated up to 5.00 mg/L, i.e. it
 *      invented numbers far beyond anything that was ever calibrated.
 *      It now interpolates exactly as the bench sketch does and CLAMPS
 *      to the calibrated span [0.00 .. 0.53] mg/L, flagging anything
 *      outside the band as an estimate ("[est]") rather than inventing
 *      a value.
 *
 *   4. SERIAL BENCH KEYS. The bench sketch's own printed guide and its
 *      if-chain disagreed ('10' / '11' can never match a single char).
 *      The guide's intent is implemented here: 7/8/9/a/b/c = 5..30 g,
 *      and 2..6 = the bench sketch's manual Motor-1 PWM levels.
 *
 *   5. LCD. The bench sketch's motor page showed BOTH motors; the
 *      production page had dropped the rotator. It is back.
 *
 *  ---------------------------------------------------------------
 *  REST CONTRACT (Supabase PostgREST — tables, not functions)
 *  ---------------------------------------------------------------
 *  Headers on every call:  apikey: <anon key> , Authorization: Bearer <anon key>
 *
 *  1) POST  /rest/v1/readings
 *       body (JSON, snake_case columns): {
 *         device_id, ph, ammonia, turbidity, turbidity_status,
 *         water_temp_c, ambient_temp_c, fan_active, fan_percent,
 *         ph_valid, turbidity_valid, water_temp_valid,
 *         ambient_valid, ammonia_valid
 *       }
 *       -> row inserted; recorded_at defaults to now().
 *       ambient_temp_c is the DHT11 (the app's "Housing Temperature").
 *
 *  2) GET   /rest/v1/commands?device_id=eq.<id>&handled=eq.false
 *            &select=id,type,grams,value,rotator_pwm,created_at
 *            &order=created_at.asc&limit=1
 *       -> [ { id, type, grams, value, rotator_pwm, created_at } ]
 *          (bare array, oldest first)
 *         - "feed_now"    { grams: 5|10|15|20|25|30, rotator_pwm }
 *         - "servo_open"  / "servo_close"   (Test Components)
 *         - "motor_test"  { value: rotator PWM 0..255, 0 stops }
 *         - "log_stream"  { value: seconds of verbose serial mirroring }
 *       One command per poll, oldest first, so a backlog drains one
 *       feed at a time instead of dumping every queued feed at once.
 *
 *  3) PATCH /rest/v1/commands?id=eq.<id>   body { "handled": true }
 *       -> marks that command handled so it is not run twice.
 *
 *  4) GET   /rest/v1/schedules?device_id=eq.<id>&active=eq.true
 *       -> [ { id, time:"HH:MM", grams, rotator_pwm, active } ]
 *       The ESP32 fires each active schedule once when the local clock
 *       (NTP, UTC+8) reaches its HH:MM.
 *
 *  5) POST  /rest/v1/device_logs    (JSON ARRAY — one batch per flush)
 *       body [ { device_id, message }, ... ]
 *       -> every line this firmware prints, mirrored so the app's Test
 *          Components console can show the same text a serial monitor
 *          does. The phone has no serial link to the board; this table
 *          IS the link. Event lines always; the sensor block only while
 *          a "log_stream" command is in force.
 *
 *  FEEDING (servo) + DELAY-ROTATION (DC rotator motor):
 *     On every feed (manual command OR schedule):
 *       1. Servo opens to SERVO_OPEN_ANGLE (160 deg).
 *       2. Held open for the gram-calibrated time (5g=400ms ... 30g=2400ms).
 *       3. Servo returns to SERVO_CLOSED_ANGLE (180 deg).
 *       4. Wait FEED_SETTLE_MS (1.5 s) for the feed to drop.
 *       5. Run the DC rotator (Motor 1) for ROTATOR_RUN_MS (12 s) at the
 *          speed the command/schedule asked for, then stop. The app
 *          offers the five PWM levels the bench sketch tested
 *          (0/90/128/191/255); 0 skips the rotation entirely.
 *
 *  TEST COMPONENTS (app Settings tab):
 *     servo_open / servo_close / motor_test drive the actuators
 *     directly. Both are bounded by the firmware, not by the app:
 *     a test-opened door force-closes after SERVO_TEST_MAX_OPEN_MS
 *     (10 s) and a test-driven motor stops after MOTOR_TEST_MAX_MS
 *     (30 s). An open feeder door keeps dropping feed, and neither
 *     actuator should keep running because somebody closed the app.
 *
 *  Libraries (Arduino IDE Library Manager):
 *     WiFi, WiFiClientSecure, HTTPClient  (ESP32 core, built in)
 *     ArduinoJson (Benoit Blanchon, v6.x)
 *     DHT sensor library (Adafruit) + Adafruit Unified Sensor
 *     OneWire, DallasTemperature
 *     LiquidCrystal_I2C
 *     ESP32Servo
 * =====================================================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>


// =====================================================
// WIFI
// =====================================================

// SSID and password are no longer stored in this file.
//
// The desktop WiFi Connector (desktop/) writes them into secrets.h,
// which is gitignored — so credentials never enter the repository or
// its history. To build by hand instead, copy secrets.h.example to
// secrets.h and fill it in.
//
// Nothing else about the WiFi logic changed: ssid/password are still
// plain const char* used by startWiFi() and checkWiFi() exactly as
// before.
#include "secrets.h"

const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;

#define WIFI_LED_PIN 2

bool lastWiFiState = false;

unsigned long lastWiFiAttempt = 0;
const unsigned long WIFI_RETRY_INTERVAL = 10000;


// =====================================================
// SUPABASE LAYER  (direct PostgREST — no server to deploy)
// =====================================================
//
// The ESP32 talks straight to Supabase's auto-generated REST API
// (PostgREST). There are NO Cloud Functions to deploy — inserts and
// reads go directly to database tables, gated by Row Level Security.
//
//   SUPABASE_URL : your project URL, e.g. "https://abcd1234.supabase.co"
//                  (Supabase dashboard -> Project Settings -> API -> Project URL)
//   SUPABASE_KEY : the project's "anon public" API key
//                  (same page -> Project API keys -> anon public)
//   DEVICE_ID    : this tank's id; must match the devices row the app
//                  creates when you tap "Pair Device".
//
// The anon key is safe to embed (it is designed to be public and is
// bounded by the RLS policies you create in the SQL editor).

#define SUPABASE_URL  "https://qlrjplwxsgklawunuoey.supabase.co"
#define SUPABASE_KEY  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InFscmpwbHd4c2drbGF3dW51b2V5Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3OTE3MDUxMTQsImV4cCI6MjEwNzI4MTExNH0.BWiTxwr6kafN5EuyyEIsQZ0Ikd7QelARsJSMkNyVxh8"
#define DEVICE_ID     "tank_01"

#define FIRMWARE_VERSION  "13.0.0"

// How often the ESP32 pushes a reading to Supabase, and polls for
// commands / schedules. These are the CLOUD cadences; the LCD and the
// local serial print update on their own faster timers below.
//
// The app treats a reading as live for 3x CLOUD_READING_INTERVAL (see
// _freshWindow in lib/core/firmware_spec.dart) — change one and change
// the other.
const unsigned long CLOUD_READING_INTERVAL  = 10000;  // push reading every 10 s
const unsigned long CLOUD_COMMAND_INTERVAL  =  5000;  // poll commands every 5 s
const unsigned long CLOUD_SCHEDULE_INTERVAL = 60000;  // refresh schedules every 60 s

unsigned long lastCloudReading  = 0;
unsigned long lastCloudCommand  = 0;
unsigned long lastCloudSchedule = 0;

// ---------------------------------------------------------------
// SERIAL LOG MIRROR  (feeds the app's Test Components console)
// ---------------------------------------------------------------
//
// Every line this firmware prints goes through logLine(), which writes
// it to Serial exactly as before AND queues it for upload to the
// `device_logs` table. The app subscribes to that table, so its console
// shows the same text as a serial monitor.
//
// Two classes of line, because a console nobody is watching must not
// fill the database:
//
//   EVENT lines (feeds, commands, schedules, WiFi changes, errors) are
//   always mirrored. They are rare, and they are the ones worth having
//   history for when you open the console later.
//
//   VERBOSE lines (the sensor block, printed every SENSOR_INTERVAL) are
//   mirrored only while the app has asked for them with a `log_stream`
//   command. The console sends that on open and renews it while it
//   stays open, so verbose output stops by itself a few minutes after
//   you navigate away.

#define MAX_LOG_LINES 24
#define MAX_LOG_LEN   120

char logBuf[MAX_LOG_LINES][MAX_LOG_LEN];
int  logCount = 0;
bool logOverflowed = false;      // lines dropped since the last flush

unsigned long lastLogFlush = 0;
const unsigned long LOG_FLUSH_INTERVAL = 3000;   // batch-insert every 3 s

// Verbose mirroring is on until this millis() deadline (0 = off).
unsigned long logStreamUntil = 0;
const unsigned long LOG_STREAM_MAX_S = 900;      // cap one request at 15 min

// A queued "Feed Now" older than this is dropped instead of dispensed.
// Without it, every feed the user tapped while the tank was offline
// would fire back-to-back the moment the ESP32 reconnects — which is
// how a tank gets a day's worth of feed in one minute.
const long FEED_COMMAND_MAX_AGE_S = 600;   // 10 minutes

// NTP (Philippines, UTC+8, no DST)
const long  NTP_GMT_OFFSET = 28800;
const int   NTP_DST_OFFSET = 0;
bool timeSynced = false;

// Cached feeding schedules fetched from Supabase.
#define MAX_SCHEDULES 12
struct FeedSchedule {
  char time[6];   // "HH:MM"
  int  grams;
  int  rotatorPwm;
  bool active;
};

FeedSchedule schedules[MAX_SCHEDULES];
int scheduleCount = 0;
int lastFedMinuteOfDay = -1;   // prevents double-firing within the same minute

// getLocalTime() blocks while it waits for the clock, so the schedule
// check runs once a second rather than on every loop pass.
unsigned long lastScheduleCheck = 0;
const unsigned long SCHEDULE_CHECK_INTERVAL = 1000;


// =====================================================
// PIN DEFINITIONS  (identical to the calibrated bench sketch)
// =====================================================

// DHT11  (HOUSING / AMBIENT temperature + humidity)
#define DHT_PIN 16
#define DHT_TYPE DHT11

// DS18B20  (WATER temperature)
#define WATER_TEMP_PIN 18

// Analog sensors (all ADC1 — safe with WiFi active)
#define TURBIDITY_PIN 34
#define PH_PIN 32
#define AMMONIA_PIN 35

// Feeder servo
#define SERVO_PIN 19

// I2C LCD
#define I2C_SDA 21
#define I2C_SCL 22


// =====================================================
// L298N MOTOR 1  ->  DC ROTATOR (runs after each feed)
// =====================================================

#define MOTOR1_EN 25
#define MOTOR1_IN1 26
#define MOTOR1_IN2 27


// =====================================================
// L298N MOTOR 2  ->  COOLING FAN (automatic, DHT11-driven)
// =====================================================

#define MOTOR2_EN 33
#define MOTOR2_IN1 17
#define MOTOR2_IN2 23


// =====================================================
// OBJECTS
// =====================================================

DHT dht(DHT_PIN, DHT_TYPE);

OneWire oneWire(WATER_TEMP_PIN);
DallasTemperature waterTempSensor(&oneWire);

LiquidCrystal_I2C lcd(0x27, 16, 2);

Servo myServo;


// =====================================================
// SERVO FEEDER CALIBRATION  (bench-sketch values)
// =====================================================
//
// 180 = CLOSED / resting.   160 = OPEN (feeding).
// Grams -> how long the door stays open:
//
//   Target grams | Delay (ms)
//   -------------+-----------
//        5       |    400
//       10       |    800
//       15       |   1200
//       20       |   1600
//       25       |   2000
//       30       |   2400

const int SERVO_OPEN_ANGLE   = 160;
const int SERVO_CLOSED_ANGLE = 180;

unsigned long gramsToOpenMs(int grams) {
  switch (grams) {
    case 5:  return 400;
    case 10: return 800;
    case 15: return 1200;
    case 20: return 1600;
    case 25: return 2000;
    case 30: return 2400;
    default: return 800;   // safe fallback ~10 g
  }
}

// Delay-rotation of the DC rotator after feed is released.
const unsigned long FEED_SETTLE_MS = 1500;   // wait 1.5 s after the door closes

// How long the DC rotator spins after each feed. 2 s was not enough to
// complete a turn; 10-15 s is. Anywhere in that range is fine.
const unsigned long ROTATOR_RUN_MS = 12000;  // 12 s

// NON-BLOCKING, and it has to be at this length. runRotator() used to
// delay() for the whole spin, which was tolerable at 2 s. At 12 s it
// would stall loop() — no WiFi service, no cloud push, no command
// poll — for the entire spin. With CLOUD_READING_INTERVAL at 10 s that
// pushes the gap between readings to ~29 s, and the app treats
// anything over 30 s as a dead device (FirmwareSpec.freshWindow). The
// feed would make the tank look like it had dropped offline.
//
// So the rotation is a deadline serviced from loop() instead.
// rotatorStopAt == 0 means idle.
unsigned long rotatorStopAt = 0;

// doFeed() is blocking (it uses delay()), so a schedule cannot interrupt
// a command feed. This flag is belt-and-braces for the same guarantee.
bool feedInProgress = false;


// =====================================================
// TEST-COMPONENT SAFETY TIMERS
// =====================================================
//
// The app's Test Components screen can drive the servo and the rotator
// directly. Both are left running until told otherwise, which is fine
// on a bench and not fine on a tank: an open feeder door keeps dropping
// feed, and a motor left on runs until the power is pulled.
//
// So a test-driven actuator always has a deadline. These do nothing to
// a normal feed, which drives both actuators itself and returns them to
// rest before it finishes.

const unsigned long SERVO_TEST_MAX_OPEN_MS = 10000;   // 10 s, then close
const unsigned long MOTOR_TEST_MAX_MS      = 30000;   // 30 s, then stop

unsigned long servoTestOpenedAt = 0;   // 0 = not opened by a test
unsigned long motorTestStartedAt = 0;  // 0 = not started by a test


// =====================================================
// TURBIDITY CALIBRATION  (SEN0189, no divider)
// =====================================================
//
// Relative turbidity % = (V_CLEAR - V) / (V_CLEAR - V_HIGH) * 100
//
// The four status bands below are voltage thresholds. Their PERCENT
// equivalents (which is what the app receives) are:
//
//   Clear            :   0.0 % .. 19.9 %
//   Slightly Turbid  :  19.9 % .. 48.5 %
//   Turbid           :  48.5 % .. 78.6 %
//   Highly Turbid    :  78.6 % .. 100.0 %
//
// The app derives those same numbers from these same constants in
// lib/core/firmware_spec.dart — keep the two in step.

const float V_CLEAR = 1.3646;   // 0.00% relative turbidity
const float V_HIGH  = 1.1916;   // 100.00% relative turbidity

const float TURBIDITY_T1 = 1.33015;  // Clear -> Slightly Turbid
const float TURBIDITY_T2 = 1.28065;  // Slightly Turbid -> Turbid
const float TURBIDITY_T3 = 1.22865;  // Turbid -> Highly Turbid


// =====================================================
// pH CALIBRATION  (3-point, carried over from the bench sketch)
// =====================================================
//
// Replace with YOUR measured buffer voltages if you recalibrate.

float voltage_pH7 = 1.950;   // reading at 6.86 buffer
float voltage_pH4 = 2.447;   // reading at 4.01 buffer
float voltage_pH9 = 1.425;   // reading at 9.18 buffer

const float PH_DIVIDER_RATIO = 2.0;


// =====================================================
// MQ-137 AMMONIA
// =====================================================
//
//   MQ-137 AO --[10k]--+--[10k]-- GND, GPIO35 reads the midpoint,
//   so GPIO35 sees half the MQ-137 output voltage.
//
// Sera reference points measured on the bench. These are the ONLY
// concentrations this sensor has ever been calibrated against, so the
// estimate is clamped to the span they cover — see readSensors().

const float AMMONIA_DIVIDER_RATIO = 2.0;

const float AMMONIA_V0 = 0.2655;   // volts at 0.00 mg/L
const float AMMONIA_V1 = 0.4030;   // volts at 0.17 mg/L
const float AMMONIA_V2 = 0.4400;   // volts at 0.53 mg/L

const float AMMONIA_C0 = 0.00;     // mg/L
const float AMMONIA_C1 = 0.17;
const float AMMONIA_C2 = 0.53;

int   ammoniaADC = 0;
float ammoniaADCVoltage = 0.0;
float ammoniaSensorVoltage = 0.0;

float ammoniaEstimatedMgL = 0.0;
bool  ammoniaInCalibrationRange = false;

// Turbidity raw values (globals so the printer / payload can read them).
int   turbidityADC = 0;
float turbidityADCVoltage = 0.0;


// =====================================================
// LIVE SENSOR VALUES + per-sensor validity
// =====================================================
//
// GRACE WINDOW — matching the bench sketch's "keep the last good
// value" behaviour. A DHT11 drops reads routinely; a DS18B20 can miss
// a conversion. Flipping a sensor to invalid on the FIRST failure made
// the app blink "Not Connected" and cut the fan mid-ramp. A sensor now
// stays valid until it has been failing for this long.

const unsigned long SENSOR_GRACE_MS = 15000;   // 15 s of silence = really gone

float waterTemperature = 0.0;
bool  waterTempValid   = false;
unsigned long lastWaterTempOk = 0;
bool  waterTempEverRead = false;

float airTemperature = 0.0;   // = HOUSING / ambient temp (DHT11)
float humidity       = 0.0;
bool  ambientValid   = false;
unsigned long lastAmbientOk = 0;
bool  ambientEverRead = false;

float  relativeTurbidity = 0.0;
String turbidityStatus   = "--";
bool   turbidityValid    = false;

int   phADC = 0;          // raw count, 0..4095
float phPinVoltage = 0.0;  // volts at the pin, before the 2:1 divider
float phVoltage = 0.0;     // reconstructed probe output
float pH = 7.0;
bool  phValid = false;

bool  ammoniaValid = false;

int servoPosition = 0;

int  motor1PWM = 0;        // DC rotator
int  motor2PWM = 0;        // cooling fan
bool fanActive = false;


// =====================================================
// LCD
// =====================================================

int lcdPage = 0;
unsigned long lastLCDUpdate = 0;
const unsigned long LCD_INTERVAL = 2500;


// =====================================================
// LOCAL SENSOR TIMING
// =====================================================

unsigned long lastSensorRead = 0;
const unsigned long SENSOR_INTERVAL = 2000;


// =====================================================
// COOLING FAN (automatic, driven by DHT11 housing temp)
// =====================================================
//
//  < 25 C : OFF
//  25-40 C: ramp 191 -> 255
//  > 40 C : full (255)
//
// Identical to the bench sketch. Mirrored in the app by
// computeFanSpeed() in lib/core/fan_speed_util.dart.

const float COOLING_START = 25.0;
const float COOLING_MAX   = 40.0;
const int   COOLING_MIN_PWM = 191;
const int   COOLING_MAX_PWM = 255;


// =====================================================
// FORWARD DECLARATIONS
// =====================================================

bool   ackCommand(const String& id);
long   commandAgeSeconds(const String& createdAt);
void   doFeed(int grams, int rotatorPwm);
void   startRotator(unsigned long ms, int pwm);
void   serviceRotator();
void   finishFeed();
String getAmmoniaStatus(float mgL);
void   logLine(const String& s, bool verbose = false);
void   flushLogs();
bool   logStreaming();


// =====================================================
// SERIAL LOG MIRROR
// =====================================================

bool logStreaming() {
  return logStreamUntil != 0 && (long)(millis() - logStreamUntil) < 0;
}

/// Print a line to Serial and queue it for the app's console.
///
/// [verbose] marks the high-volume lines (the sensor block): they reach
/// Serial always, but the cloud only while streaming is switched on.
void logLine(const String& s, bool verbose) {
  Serial.println(s);

  if (verbose && !logStreaming()) return;

  if (logCount >= MAX_LOG_LINES) {
    // Keep the OLDEST lines and note the gap. Dropping the newest is
    // the wrong way round for a log you are reading after the fact —
    // but either way the console is told, rather than silently showing
    // a log with holes in it.
    logOverflowed = true;
    return;
  }

  strncpy(logBuf[logCount], s.c_str(), MAX_LOG_LEN - 1);
  logBuf[logCount][MAX_LOG_LEN - 1] = '\0';

  // Truncating at a fixed byte count can cut a multi-byte UTF-8
  // character in half, and several lines here contain an em-dash
  // ("[est — outside calibrated band]"). A half character is invalid
  // UTF-8, Postgres rejects the whole INSERT with "invalid byte
  // sequence for encoding UTF8", and the ENTIRE batch of log lines is
  // lost — silently, since flushLogs() cannot log its own failure.
  //
  // So drop a trailing sequence only when it is actually INCOMPLETE.
  // Find the lead byte of the last character, work out how many bytes
  // that character needs, and cut only if the buffer ran out before it
  // finished. (Naively stripping continuation bytes would destroy a
  // perfectly valid character that happens to land at the end.)
  char* b = logBuf[logCount];
  const int n = strlen(b);
  int i = n - 1;
  while (i >= 0 && ((unsigned char)b[i] & 0xC0) == 0x80) i--;   // to the lead byte
  if (i >= 0) {
    const unsigned char lead = (unsigned char)b[i];
    int need;
    if      ((lead & 0x80) == 0x00) need = 1;   // ASCII
    else if ((lead & 0xE0) == 0xC0) need = 2;
    else if ((lead & 0xF0) == 0xE0) need = 3;   // em-dash lives here
    else if ((lead & 0xF8) == 0xF0) need = 4;
    else                            need = 1;   // stray byte; leave as-is
    if (i + need > n) b[i] = '\0';              // incomplete -> drop it
  }

  logCount++;
}

/// JSON string escaping — quotes, backslashes and control characters.
/// Without this a log line containing a quote would produce a malformed
/// body and PostgREST would reject the whole batch.
String jsonEscape(const char* s) {
  String out;
  out.reserve(strlen(s) + 8);
  for (const char* p = s; *p; p++) {
    switch (*p) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((unsigned char)*p < 0x20) {
          char esc[7];
          snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)*p);
          out += esc;
        } else {
          out += *p;
        }
    }
  }
  return out;
}

/// Bulk-insert the queued lines. PostgREST takes a JSON array, so the
/// whole batch is one request rather than one per line.
void flushLogs() {
  if (logCount == 0 && !logOverflowed) return;
  if (WiFi.status() != WL_CONNECTED) {
    // Nothing can be sent, so drop the queue rather than letting it
    // block every later line until the WiFi returns.
    logCount = 0;
    logOverflowed = false;
    return;
  }

  String body = "[";
  if (logOverflowed) {
    body += "{\"device_id\":\"" DEVICE_ID "\",\"message\":\"";
    body += "[lines dropped — device log buffer full]\"},";
  }
  for (int i = 0; i < logCount; i++) {
    if (i) body += ",";
    body += "{\"device_id\":\"" DEVICE_ID "\",\"message\":\"";
    body += jsonEscape(logBuf[i]);
    body += "\"}";
  }
  body += "]";

  // Clear the queue BEFORE the request: a failed upload must not make
  // the same lines retry forever, and logLine() is free to fill the
  // buffer again while this is in flight.
  logCount = 0;
  logOverflowed = false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/device_logs";
  if (!http.begin(client, url)) return;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Prefer", "return=minimal");
  http.setTimeout(10000);

  const int code = http.POST(body);
  if (!(code == 200 || code == 201 || code == 204)) {
    // Serial only — logging a failed log upload through logLine() would
    // queue a line that the next flush tries to send, and so on.
    Serial.print("[Cloud] device_logs insert -> HTTP ");
    Serial.println(code);
  }
  http.end();
}


// =====================================================
// WIFI
// =====================================================

void startWiFi() {
  Serial.println();
  Serial.println("================================");
  Serial.println("WIFI CONNECTION");
  Serial.println("================================");
  Serial.print("SSID: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  digitalWrite(WIFI_LED_PIN, LOW);

  Serial.println("WiFi: connecting...");
  WiFi.begin(ssid, password);
  lastWiFiAttempt = millis();
}

void checkWiFi() {
  bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected) {
    digitalWrite(WIFI_LED_PIN, HIGH);
    if (!lastWiFiState) {
      lastWiFiState = true;
      logLine("WiFi: connected");
      logLine("IP address: " + WiFi.localIP().toString());
      logLine("Signal strength: " + String(WiFi.RSSI()) + " dBm");

      // (Re)sync the clock each time we regain WiFi.
      configTime(NTP_GMT_OFFSET, NTP_DST_OFFSET, "pool.ntp.org", "time.nist.gov");
    }
    return;
  }

  digitalWrite(WIFI_LED_PIN, LOW);
  if (lastWiFiState) {
    lastWiFiState = false;
    // Serial only: there is no link to send it over right now, and by
    // the time there is, the "connected" line says the same thing.
    Serial.println();
    Serial.println("WiFi: disconnected");
  }

  if (millis() - lastWiFiAttempt >= WIFI_RETRY_INTERVAL) {
    lastWiFiAttempt = millis();
    Serial.println();
    Serial.println("WiFi: retrying...");
    WiFi.disconnect();
    delay(100);
    WiFi.begin(ssid, password);
  }
}


// =====================================================
// READ SENSORS
// =====================================================
//
// Formulas here are the bench sketch's, unchanged. What is added is the
// per-sensor validity the app needs in order to show a real "Not
// Connected" state instead of a stale number — with the grace window
// described at SENSOR_GRACE_MS so a single dropped read is not treated
// as a disconnected sensor.

void readSensors() {

  const unsigned long now = millis();

  // ---------------- DS18B20 water temp ----------------
  waterTempSensor.requestTemperatures();
  float wt = waterTempSensor.getTempCByIndex(0);
  const bool wtOk = (wt != DEVICE_DISCONNECTED_C && wt > -50.0 && wt < 100.0);
  if (wtOk) {
    waterTemperature  = wt;          // keep the last good value (bench behaviour)
    lastWaterTempOk   = now;
    waterTempEverRead = true;
  }
  waterTempValid = waterTempEverRead && (now - lastWaterTempOk <= SENSOR_GRACE_MS);

  // ---------------- DHT11 housing/ambient -------------
  float newHumidity       = dht.readHumidity();
  float newAirTemperature = dht.readTemperature();
  const bool dhtOk = (!isnan(newHumidity) && !isnan(newAirTemperature));
  if (dhtOk) {
    humidity        = newHumidity;   // keep the last good value (bench behaviour)
    airTemperature  = newAirTemperature;
    lastAmbientOk   = now;
    ambientEverRead = true;
  }
  ambientValid = ambientEverRead && (now - lastAmbientOk <= SENSOR_GRACE_MS);

  // ---------------- Turbidity (SEN0189) ---------------
  turbidityADC = analogRead(TURBIDITY_PIN);
  turbidityADCVoltage = (turbidityADC / 4095.0) * 3.3;
  // Any ADC level is a legitimate turbidity reading — a blocked sensor
  // (very turbid) can read a very low voltage, which must still show as
  // 100%, not "Not Connected". Whole-device offline is handled by the
  // app's connection state, so turbidity itself is always valid here.
  turbidityValid = true;

  relativeTurbidity =
    ((V_CLEAR - turbidityADCVoltage) / (V_CLEAR - V_HIGH)) * 100.0;
  relativeTurbidity = constrain(relativeTurbidity, 0.0, 100.0);

  if      (turbidityADCVoltage >= TURBIDITY_T1) turbidityStatus = "Clear";
  else if (turbidityADCVoltage >= TURBIDITY_T2) turbidityStatus = "Slightly Turbid";
  else if (turbidityADCVoltage >= TURBIDITY_T3) turbidityStatus = "Turbid";
  else                                          turbidityStatus = "Highly Turbid";

  // ---------------- pH (3-point) ----------------------
  phADC = analogRead(PH_PIN);
  const float phADCVoltage = (phADC / 4095.0) * 3.3;
  phPinVoltage = phADCVoltage;
  phVoltage = phADCVoltage * PH_DIVIDER_RATIO;
  // A probe that is unplugged reads pinned at one rail; anything inside
  // the rails is a real measurement.
  phValid = (phADCVoltage > 0.05 && phADCVoltage < 3.25);

  if (phVoltage >= voltage_pH7) {
    pH = 7.0 + (phVoltage - voltage_pH7) * (4.0 - 7.0) /
               (voltage_pH4 - voltage_pH7);
  } else {
    pH = 7.0 + (phVoltage - voltage_pH7) * (9.18 - 7.0) /
               (voltage_pH9 - voltage_pH7);
  }
  pH = constrain(pH, 0.0, 14.0);

  // ---------------- MQ-137 ammonia --------------------
  ammoniaADC = analogRead(AMMONIA_PIN);
  ammoniaADCVoltage    = (ammoniaADC / 4095.0) * 3.3;
  ammoniaSensorVoltage = ammoniaADCVoltage * AMMONIA_DIVIDER_RATIO;
  // "Valid" = the sensor is electrically present (reading something).
  // This does NOT depend on the calibration band, so the app shows a
  // number instead of "Not Connected" when the MQ-137 sits outside it.
  ammoniaValid = (ammoniaADCVoltage > 0.02);

  // NH4/NH3 estimate (mg/L) — the bench sketch's two-segment
  // interpolation across the Sera reference points, CLAMPED to the span
  // those points cover. Outside the band the reading is reported at the
  // nearest calibrated end and flagged "[est]"; this firmware does not
  // extrapolate a concentration it has never been calibrated for.
  const float v = ammoniaSensorVoltage;
  ammoniaInCalibrationRange = (v >= AMMONIA_V0 && v <= AMMONIA_V2);

  if (v <= AMMONIA_V0) {
    ammoniaEstimatedMgL = AMMONIA_C0;
  } else if (v <= AMMONIA_V1) {
    ammoniaEstimatedMgL = AMMONIA_C0 +
        (v - AMMONIA_V0) * (AMMONIA_C1 - AMMONIA_C0) / (AMMONIA_V1 - AMMONIA_V0);
  } else if (v <= AMMONIA_V2) {
    ammoniaEstimatedMgL = AMMONIA_C1 +
        (v - AMMONIA_V1) * (AMMONIA_C2 - AMMONIA_C1) / (AMMONIA_V2 - AMMONIA_V1);
  } else {
    ammoniaEstimatedMgL = AMMONIA_C2;   // above the top reference: clamp, don't invent
  }
}


// =====================================================
// COOLING FAN (Motor 2) — automatic
// =====================================================

void controlCoolingMotor() {
  int pwm = 0;

  // Same ramp as the bench sketch, driven by the last-good housing
  // temperature. Only a genuinely dead DHT11 (silent past the grace
  // window) holds the fan OFF — a single dropped read must not cut
  // cooling, which is what used to happen here.
  if (!ambientValid) {
    pwm = 0;
  } else if (airTemperature < COOLING_START) {
    pwm = 0;
  } else if (airTemperature < COOLING_MAX) {
    pwm = map(airTemperature * 10, COOLING_START * 10, COOLING_MAX * 10,
              COOLING_MIN_PWM, COOLING_MAX_PWM);
  } else {
    pwm = COOLING_MAX_PWM;
  }

  pwm = constrain(pwm, 0, 255);
  motor2PWM = pwm;
  fanActive = (pwm > 0);
  analogWrite(MOTOR2_EN, motor2PWM);
}


// =====================================================
// DC ROTATOR (Motor 1) — runs after each feed
// =====================================================

/// Run the rotator at [pwm] for [ms], then stop. [pwm] comes from the
/// app's rotator-speed selection; 0 means "do not rotate at all", which
/// is a legitimate choice for a feed.
/// Start the post-feed rotation and return immediately. The feed is
/// only finished once serviceRotator() stops the motor, so
/// feedInProgress stays true until then.
void startRotator(unsigned long ms, int pwm) {
  pwm = constrain(pwm, 0, 255);

  if (pwm == 0 || ms == 0) {
    logLine("[Feed] Rotator -> skipped (speed set to Off)");
    finishFeed();
    return;
  }

  logLine("[Feed] Rotator -> ON at " + String((int)((pwm / 255.0) * 100)) +
          "% for " + String(ms / 1000.0, 1) + " s");
  motor1PWM = pwm;
  analogWrite(MOTOR1_EN, motor1PWM);

  rotatorStopAt = millis() + ms;
  // millis() wraps to 0 after ~49 days and 0 is the idle sentinel.
  if (rotatorStopAt == 0) rotatorStopAt = 1;
}

/// Stop the rotator once its deadline passes. Called every loop pass.
void serviceRotator() {
  if (rotatorStopAt == 0) return;
  if ((long)(millis() - rotatorStopAt) < 0) return;

  rotatorStopAt = 0;
  motor1PWM = 0;
  analogWrite(MOTOR1_EN, motor1PWM);
  logLine("[Feed] Rotator -> OFF");
  finishFeed();
}

/// Release the feed lock and hand the LCD back to its page cycle.
void finishFeed() {
  logLine("[Feed] Cycle complete");
  lastLCDUpdate = 0;
  feedInProgress = false;
}


// =====================================================
// TEST COMPONENTS: direct actuator control
// =====================================================

void servoTest(bool open) {
  if (feedInProgress) {
    logLine("[Test] Servo ignored — a feed is running");
    return;
  }

  if (open) {
    myServo.write(SERVO_OPEN_ANGLE);
    servoPosition = SERVO_OPEN_ANGLE;
    servoTestOpenedAt = millis();
    logLine("[Test] Servo -> OPEN " + String(SERVO_OPEN_ANGLE) + " degrees");
  } else {
    myServo.write(SERVO_CLOSED_ANGLE);
    servoPosition = SERVO_CLOSED_ANGLE;
    servoTestOpenedAt = 0;
    logLine("[Test] Servo -> CLOSED " + String(SERVO_CLOSED_ANGLE) + " degrees");
  }
}

void motorTest(int pwm) {
  if (feedInProgress) {
    logLine("[Test] Motor ignored — a feed is running");
    return;
  }

  motor1PWM = constrain(pwm, 0, 255);
  analogWrite(MOTOR1_EN, motor1PWM);
  motorTestStartedAt = (motor1PWM > 0) ? millis() : 0;

  logLine("[Test] Rotator -> " + String(motor1PWM) + "/255 (" +
          String((int)((motor1PWM / 255.0) * 100)) + "%)");
}

/// Enforce the deadlines above. Called every loop pass.
void checkTestTimeouts() {
  if (servoTestOpenedAt != 0 &&
      millis() - servoTestOpenedAt >= SERVO_TEST_MAX_OPEN_MS) {
    myServo.write(SERVO_CLOSED_ANGLE);
    servoPosition = SERVO_CLOSED_ANGLE;
    servoTestOpenedAt = 0;
    logLine("[Test] Servo auto-closed after 10 s");
  }

  if (motorTestStartedAt != 0 &&
      millis() - motorTestStartedAt >= MOTOR_TEST_MAX_MS) {
    motor1PWM = 0;
    analogWrite(MOTOR1_EN, motor1PWM);
    motorTestStartedAt = 0;
    logLine("[Test] Rotator auto-stopped after 30 s");
  }
}


// =====================================================
// FEED CYCLE: servo drop -> settle -> rotate
// =====================================================

void doFeed(int grams, int rotatorPwm) {
  if (feedInProgress) {
    logLine("[Feed] Ignored — a feed is already running");
    return;
  }
  feedInProgress = true;

  // A feed owns both actuators, so cancel any test deadline rather than
  // letting a timer fire part-way through the cycle.
  servoTestOpenedAt = 0;
  motorTestStartedAt = 0;

  unsigned long openMs = gramsToOpenMs(grams);

  logLine("[Feed] Dispensing " + String(grams) + " g  (open " +
          String(openMs) + " ms, rotator " +
          String((int)((constrain(rotatorPwm, 0, 255) / 255.0) * 100)) + "%)");

  // The whole cycle blocks for up to ~6 s, so the LCD would otherwise
  // sit frozen on whatever page it happened to be showing. Say what is
  // happening instead — this is the one moment the machine moves.
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Feeding ");
  lcd.print(grams);
  lcd.print("g");
  lcd.setCursor(0, 1);
  lcd.print("Dispensing...");

  // 1-3: servo opens, holds for the gram time, closes.
  myServo.write(SERVO_OPEN_ANGLE);
  servoPosition = SERVO_OPEN_ANGLE;
  delay(openMs);
  myServo.write(SERVO_CLOSED_ANGLE);
  servoPosition = SERVO_CLOSED_ANGLE;

  // 4: wait for the feed to drop.
  logLine("[Feed] Settling 1.5 s before rotating...");
  lcd.setCursor(0, 1);
  lcd.print("Settling...  ");
  delay(FEED_SETTLE_MS);

  // 5: start the rotation and return. serviceRotator() stops the
  // motor when its deadline passes and clears feedInProgress then —
  // loop() keeps running throughout, so the cloud layer stays alive.
  lcd.setCursor(0, 1);
  lcd.print("Rotating...  ");
  startRotator(ROTATOR_RUN_MS, rotatorPwm);
}


// =====================================================
// CLOUD: push one reading
// =====================================================

void pushReading() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();   // thesis simplicity; pin the CA cert for production-grade TLS

  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/readings";
  if (!http.begin(client, url)) {
    Serial.println("[Cloud] readings insert begin() failed");
    return;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Prefer", "return=minimal");
  http.setTimeout(10000);

  // Postgres columns are snake_case.
  StaticJsonDocument<512> doc;
  doc["device_id"]        = DEVICE_ID;
  doc["ph"]               = serialized(String(pH, 2));
  doc["ammonia"]          = serialized(String(ammoniaEstimatedMgL, 2));  // mg/L
  doc["turbidity"]        = serialized(String(relativeTurbidity, 1));    // percent
  doc["turbidity_status"] = turbidityStatus;
  doc["water_temp_c"]     = serialized(String(waterTemperature, 1));
  doc["ambient_temp_c"]   = serialized(String(airTemperature, 1));       // Housing temp
  doc["fan_active"]       = fanActive;
  doc["fan_percent"]      = (int)((motor2PWM / 255.0) * 100.0);
  doc["ph_valid"]         = phValid;
  doc["turbidity_valid"]  = turbidityValid;
  doc["water_temp_valid"] = waterTempValid;
  doc["ambient_valid"]    = ambientValid;
  doc["ammonia_valid"]    = ammoniaValid;

  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  if (code == 200 || code == 201 || code == 204) {
    // Verbose: one of these every 10 s forever, so it only reaches the
    // app's console while somebody is watching it.
    logLine("[Cloud] Reading stored", true);
  } else {
    logLine("[Cloud] readings insert -> HTTP " + String(code));
    logLine(http.getString());   // PostgREST prints the reason
  }
  http.end();
}


// =====================================================
// CLOUD: poll + execute the next pending command
// =====================================================
//
// One command per poll, oldest first. Each command is marked handled
// BEFORE the feed runs: a reset or brown-out mid-dispense then loses
// that one feed instead of replaying it on every boot. For a feeder,
// under-feeding once is the recoverable failure; repeat-feeding is not.

void pollCommands() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (feedInProgress) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = String(SUPABASE_URL) +
               "/rest/v1/commands?select=id,type,grams,value,rotator_pwm,created_at"
               "&device_id=eq." + DEVICE_ID +
               "&handled=eq.false&order=created_at.asc&limit=1";
  if (!http.begin(client, url)) return;
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.setTimeout(10000);

  int code = http.GET();
  if (code != 200) {
    http.end();
    return;
  }

  String body = http.getString();
  http.end();

  // PostgREST returns a bare JSON array, e.g. [ { "id": "...", ... } ].
  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, body)) return;

  JsonArray cmds = doc.as<JsonArray>();
  if (cmds.isNull() || cmds.size() == 0) return;

  JsonObject c = cmds[0];
  String id   = c["id"].as<String>();
  String type = c["type"].as<String>();

  // Mark it handled first; if that PATCH fails, leave the command alone
  // and retry next poll rather than dispensing a feed we cannot ack.
  if (!ackCommand(id)) {
    Serial.println("[Cmd] ack failed — not running this command yet");
    return;
  }

  // ---------------- feed ----------------
  if (type == "feed_now") {
    const long ageS = commandAgeSeconds(c["created_at"].as<String>());
    if (ageS >= 0 && ageS > FEED_COMMAND_MAX_AGE_S) {
      logLine("[Cmd] feed_now dropped — queued " + String(ageS) + " s ago (stale)");
      return;
    }

    const int grams = c["grams"] | 10;
    // A row written before rotator_pwm existed sends null; fall back to
    // what this firmware always used to do, which is full speed.
    const int pwm = c["rotator_pwm"] | 255;
    logLine("[Cmd] feed_now " + String(grams) + " g");
    doFeed(grams, pwm);
    return;
  }

  // ---------------- Test Components ----------------
  //
  // These are deliberately NOT age-checked: they are only ever sent
  // from a screen somebody is looking at, and the actuator deadlines in
  // checkTestTimeouts() already bound how long one can have an effect.

  if (type == "servo_open") {
    servoTest(true);
    return;
  }

  if (type == "servo_close") {
    servoTest(false);
    return;
  }

  if (type == "motor_test") {
    motorTest(c["value"] | 0);
    return;
  }

  if (type == "log_stream") {
    long secs = c["value"] | 0;
    if (secs <= 0) {
      logStreamUntil = 0;
      logLine("[Cmd] log_stream off");
    } else {
      if (secs > (long)LOG_STREAM_MAX_S) secs = LOG_STREAM_MAX_S;
      logStreamUntil = millis() + (unsigned long)secs * 1000UL;
      // millis() rolls over to 0 after ~49 days; a deadline of exactly
      // 0 is the sentinel for "off", so nudge past it.
      if (logStreamUntil == 0) logStreamUntil = 1;
      logLine("[Cmd] log_stream on for " + String(secs) + " s");
    }
    return;
  }

  logLine("[Cmd] unknown type: " + type);
}

/// Age of a PostgREST `created_at` timestamp in seconds, or -1 when the
/// clock has not synced yet / the timestamp cannot be parsed (in which
/// case the caller treats the command as fresh rather than dropping it).
long commandAgeSeconds(const String& createdAt) {
  if (!timeSynced || createdAt.length() < 19) return -1;

  struct tm t = {};
  // "2026-10-17T09:41:05.123456+00:00" — parse the UTC calendar part.
  if (sscanf(createdAt.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d",
             &t.tm_year, &t.tm_mon, &t.tm_mday,
             &t.tm_hour, &t.tm_min, &t.tm_sec) != 6) {
    return -1;
  }
  t.tm_year -= 1900;
  t.tm_mon  -= 1;
  t.tm_isdst = 0;

  // created_at is UTC, but mktime() reads a struct tm as LOCAL time.
  // Since local is UTC+8, mktime() returns (true epoch - 28800), so the
  // offset is added back rather than subtracted. Getting this sign
  // wrong puts every command 16 hours into the past, which would make
  // the staleness check below drop all of them.
  time_t created = mktime(&t) + NTP_GMT_OFFSET;
  time_t now = time(nullptr);
  if (now <= 0 || created <= 0) return -1;

  const long age = (long)difftime(now, created);
  // A negative age means the device clock is behind the server's, not
  // that the command is from the future — treat it as fresh.
  return age < 0 ? 0 : age;
}

bool ackCommand(const String& id) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  // PATCH the one row: mark it handled so it isn't run twice.
  String url = String(SUPABASE_URL) + "/rest/v1/commands?id=eq." + id;
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Prefer", "return=minimal");
  http.setTimeout(10000);

  // Only `handled` is set here. handled_at is left to the database (a
  // JSON string like "now()" is not a timestamptz literal, so sending
  // one would make PostgREST reject the whole PATCH with a 400).
  String payload = "{\"handled\":true}";
  int code = http.sendRequest("PATCH", payload);   // no PATCH() helper on all cores
  http.end();

  const bool ok = (code == 200 || code == 204);
  if (!ok) {
    Serial.print("[Cloud] command ack -> HTTP ");
    Serial.println(code);
  }
  return ok;
}


// =====================================================
// CLOUD: refresh feeding schedules
// =====================================================

void refreshSchedules() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = String(SUPABASE_URL) +
               "/rest/v1/schedules?select=id,time,grams,rotator_pwm,active"
               "&device_id=eq." + DEVICE_ID + "&active=eq.true";
  if (!http.begin(client, url)) return;
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.setTimeout(10000);

  int code = http.GET();
  if (code != 200) {
    http.end();
    return;
  }

  String body = http.getString();
  http.end();

  // PostgREST returns a bare JSON array.
  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, body)) return;

  JsonArray arr = doc.as<JsonArray>();
  scheduleCount = 0;
  for (JsonObject s : arr) {
    if (scheduleCount >= MAX_SCHEDULES) break;
    String t = s["time"].as<String>();
    strncpy(schedules[scheduleCount].time, t.c_str(), 5);
    schedules[scheduleCount].time[5] = '\0';
    schedules[scheduleCount].grams      = s["grams"] | 10;
    schedules[scheduleCount].rotatorPwm = s["rotator_pwm"] | 255;
    schedules[scheduleCount].active     = s["active"] | true;
    scheduleCount++;
  }
  logLine("[Cloud] Schedules loaded: " + String(scheduleCount));
}


// =====================================================
// SCHEDULED FEEDING  (fires each active schedule at its HH:MM)
// =====================================================

void checkScheduledFeeding() {
  struct tm t;
  if (!getLocalTime(&t, 50)) return;   // clock not ready yet
  timeSynced = true;

  int minuteOfDay = t.tm_hour * 60 + t.tm_min;
  if (minuteOfDay == lastFedMinuteOfDay) return;   // already handled this minute

  char now[6];
  snprintf(now, sizeof(now), "%02d:%02d", t.tm_hour, t.tm_min);

  for (int i = 0; i < scheduleCount; i++) {
    if (!schedules[i].active) continue;
    if (strcmp(schedules[i].time, now) == 0) {
      logLine("[Schedule] " + String(now) + " -> feed " +
              String(schedules[i].grams) + " g");
      lastFedMinuteOfDay = minuteOfDay;   // claim the minute BEFORE the
                                          // blocking feed, so two rows at
                                          // the same HH:MM cannot double-feed
      doFeed(schedules[i].grams, schedules[i].rotatorPwm);
      return;
    }
  }
}


// =====================================================
// MOTOR 1 — manual helper (bench-sketch serial testing)
// =====================================================

void setMotor1(int pwm) {
  motor1PWM = constrain(pwm, 0, 255);
  analogWrite(MOTOR1_EN, motor1PWM);
  // Same deadline as a motor_test from the app: a bench key must not
  // leave the rotator running either.
  motorTestStartedAt = (motor1PWM > 0) ? millis() : 0;
  logLine("Motor 1 (rotator) PWM: " + String(motor1PWM) + " / 255   " +
          String((int)((motor1PWM / 255.0) * 100.0)) + "%");
}


// =====================================================
// SERIAL COMMANDS (local bench testing only — the real
// control path is the mobile app via Supabase)
// =====================================================
//
// Key map matches the bench sketch's PRINTED guide (its if-chain
// compared single chars against "10"/"11", which can never match):
//
//   FEED:   7=5g  8=10g  9=15g  a=20g  b=25g  c=30g
//   MOTOR1: 2=OFF 3=25%  4=50%  5=75%  6=100%

void checkSerialCommand() {
  if (!Serial.available()) return;
  char command = Serial.read();

  // Bench feeds use full rotator speed, matching what the bench sketch
  // does. The app is where the speed becomes selectable.
  if      (command == '7') doFeed(5,  255);
  else if (command == '8') doFeed(10, 255);
  else if (command == '9') doFeed(15, 255);
  else if (command == 'a') doFeed(20, 255);
  else if (command == 'b') doFeed(25, 255);
  else if (command == 'c') doFeed(30, 255);
  else if (command == '2') setMotor1(0);
  else if (command == '3') setMotor1(90);
  else if (command == '4') setMotor1(128);
  else if (command == '5') setMotor1(191);
  else if (command == '6') setMotor1(255);
}


// =====================================================
// AMMONIA STATUS (mg/L -> label), used on LCD + serial
// =====================================================
//
// Bench-sketch bands. Mirrored by the app in firmware_spec.dart.

String getAmmoniaStatus(float mgL) {
  if (mgL < 0.25) return "Safe";
  if (mgL < 0.50) return "Warning";
  return "Critical";
}


// =====================================================
// SERIAL SENSOR MONITOR
// =====================================================

// Built as whole lines so each one can go to Serial AND to the app's
// console through logLine(). These are the high-volume lines (every
// SENSOR_INTERVAL), so they are marked verbose: Serial always gets
// them, the cloud only while log streaming is on.
#define LOG_V(s) logLine((s), true)

void printSensorData() {
  Serial.println();
  LOG_V("========== SENSOR DATA ==========");

  LOG_V("Water Temp: " +
        (waterTempValid ? String(waterTemperature) + " C" : String("Not Connected")));

  LOG_V("Housing Temp: " +
        (ambientValid ? String(airTemperature) + " C" : String("Not Connected")));

  LOG_V("Humidity:   " +
        (ambientValid ? String(humidity) + " %" : String("Not Connected")));

  if (turbidityValid) {
    LOG_V("Turbidity:  " + String(relativeTurbidity, 2) + "% (" + turbidityStatus +
          ")   |   ADC: " + String(turbidityADC) +
          "   ADC V: " + String(turbidityADCVoltage, 3) + " V");
  } else {
    LOG_V("Turbidity:  Not Connected");
  }

  if (phValid) {
    LOG_V("pH:         " + String(pH, 2) +
          "   |   ADC: " + String(phADC) +
          "   pin: " + String(phPinVoltage, 3) + " V" +
          "   probe: " + String(phVoltage, 3) + " V");
  } else {
    LOG_V("pH:         Not Connected   |   ADC: " + String(phADC) +
          "   pin: " + String(phPinVoltage, 3) + " V");
  }

  if (ammoniaValid) {
    String line = "Ammonia:    " + String(ammoniaEstimatedMgL, 2) + " mg/L (" +
                  getAmmoniaStatus(ammoniaEstimatedMgL) + ")";
    if (!ammoniaInCalibrationRange) line += " [est — outside calibrated band]";
    line += "   |   Sensor: " + String(ammoniaSensorVoltage, 3) + " V";
    LOG_V(line);
  } else {
    LOG_V("Ammonia:    Not Connected");
  }

  LOG_V("Fan:        " + String(fanActive ? "ON " : "OFF ") +
        String((int)((motor2PWM / 255.0) * 100.0)) + "%");

  LOG_V("Rotator:    " + String((int)((motor1PWM / 255.0) * 100.0)) + "%");

  LOG_V("=================================");
}


// =====================================================
// LCD DISPLAY
// =====================================================

void updateLCD() {
  lcd.clear();

  switch (lcdPage) {

    case 0:  // Water temp + pH
      lcd.setCursor(0, 0);
      lcd.print("Water:");
      if (waterTempValid) { lcd.print(waterTemperature, 1); lcd.print((char)223); lcd.print("C"); }
      else lcd.print("--");
      lcd.setCursor(0, 1);
      lcd.print("pH:");
      if (phValid) lcd.print(pH, 2); else lcd.print("--");
      break;

    case 1:  // Housing temp + humidity
      lcd.setCursor(0, 0);
      lcd.print("House:");
      if (ambientValid) { lcd.print(airTemperature, 1); lcd.print((char)223); lcd.print("C"); }
      else lcd.print("--");
      lcd.setCursor(0, 1);
      lcd.print("Hum:");
      if (ambientValid) { lcd.print(humidity, 1); lcd.print("%"); }
      else lcd.print("--");
      break;

    case 2:  // Turbidity
      lcd.setCursor(0, 0);
      lcd.print(turbidityValid ? turbidityStatus : "Turbidity --");
      lcd.setCursor(0, 1);
      if (turbidityValid) { lcd.print(relativeTurbidity, 1); lcd.print("% turbid"); }
      else lcd.print("Not Connected");
      break;

    case 3:  // Ammonia
      lcd.setCursor(0, 0);
      lcd.print("Ammonia mg/L");
      lcd.setCursor(0, 1);
      if (ammoniaValid) {
        lcd.print(ammoniaEstimatedMgL, 2);
        lcd.print(" ");
        lcd.print(getAmmoniaStatus(ammoniaEstimatedMgL));
      } else lcd.print("Not Connected");
      break;

    case 4:  // Both motors, as on the bench sketch's motor page
      lcd.setCursor(0, 0);
      lcd.print("Cool:");
      lcd.print((int)((motor2PWM / 255.0) * 100.0));
      lcd.print("% ");
      lcd.print(fanActive ? "ON" : "OFF");
      lcd.setCursor(0, 1);
      lcd.print("Rotator:");
      lcd.print((int)((motor1PWM / 255.0) * 100.0));
      lcd.print("%");
      break;
  }

  lcdPage++;
  if (lcdPage > 4) lcdPage = 0;
}


// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(WIFI_LED_PIN, OUTPUT);
  digitalWrite(WIFI_LED_PIN, LOW);

  startWiFi();

  Serial.println();
  Serial.println("================================");
  Serial.println("CRX-PULSE (0o0)");
  Serial.print("Firmware v"); Serial.println(FIRMWARE_VERSION);
  Serial.println("================================");

  // ADC
  analogReadResolution(12);
  analogSetPinAttenuation(TURBIDITY_PIN, ADC_11db);
  analogSetPinAttenuation(PH_PIN,        ADC_11db);
  analogSetPinAttenuation(AMMONIA_PIN,   ADC_11db);

  // Sensors
  dht.begin();
  waterTempSensor.begin();
  // 11-bit = 0.125 C steps and a ~375 ms conversion instead of ~750 ms.
  // The reading is displayed to 0.1 C, so nothing is lost, and the
  // 2-second sensor pass no longer spends most of its time blocked.
  waterTempSensor.setResolution(11);

  // LCD
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  delay(100);
  lcd.init();
  delay(100);
  lcd.backlight();
  delay(100);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("CRX-PULSE *(0o0)*");
  lcd.setCursor(0, 1);
  lcd.print("Starting...");
  delay(1500);
  lcd.clear();

  // Servo: reserve an LEDC timer BEFORE the analogWrite() motors, then
  // settle CLOSED so there is no sweep from 0 at power-on.
  ESP32PWM::allocateTimer(0);
  myServo.setPeriodHertz(50);
  myServo.attach(SERVO_PIN, 500, 2400);
  myServo.write(SERVO_CLOSED_ANGLE);
  servoPosition = SERVO_CLOSED_ANGLE;

  // Motor 1 (DC rotator)
  pinMode(MOTOR1_EN, OUTPUT);
  pinMode(MOTOR1_IN1, OUTPUT);
  pinMode(MOTOR1_IN2, OUTPUT);
  digitalWrite(MOTOR1_IN1, LOW);
  digitalWrite(MOTOR1_IN2, HIGH);   // rotator spin direction
  analogWrite(MOTOR1_EN, 0);

  // Motor 2 (cooling fan)
  pinMode(MOTOR2_EN, OUTPUT);
  pinMode(MOTOR2_IN1, OUTPUT);
  pinMode(MOTOR2_IN2, OUTPUT);
  digitalWrite(MOTOR2_IN1, HIGH);
  digitalWrite(MOTOR2_IN2, LOW);
  analogWrite(MOTOR2_EN, 0);

  // Clock for scheduled feeding
  configTime(NTP_GMT_OFFSET, NTP_DST_OFFSET, "pool.ntp.org", "time.nist.gov");

  Serial.println();
  Serial.println("Cloud: Supabase (direct PostgREST, anon key)");
  Serial.println("Mobile commands: feed_now {grams}");
  Serial.println();
  Serial.println("SERIAL BENCH KEYS");
  Serial.println("----------------------------");
  Serial.println("FEED:   7=5g  8=10g  9=15g  a=20g  b=25g  c=30g");
  Serial.println("MOTOR1: 2=OFF 3=25%  4=50%  5=75%  6=100%");
  Serial.println("Motor 2 (cooling fan) = DHT11 AUTOMATIC");
  Serial.println("MQ-137 = GPIO35 ANALOG");
  Serial.println("----------------------------");
}


// =====================================================
// MAIN LOOP
// =====================================================

void loop() {
  checkWiFi();
  checkSerialCommand();

  // Stop the post-feed rotation when its deadline passes.
  serviceRotator();

  // Close the feeder door / stop the rotator if a Test Components
  // command left either running. Must be before anything that blocks.
  checkTestTimeouts();

  // Local sensor refresh + fan control + serial print + LCD
  if (millis() - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = millis();
    readSensors();
    controlCoolingMotor();
    printSensorData();
  }

  if (millis() - lastLCDUpdate >= LCD_INTERVAL) {
    lastLCDUpdate = millis();
    updateLCD();
  }

  // Cloud: push reading
  if (millis() - lastCloudReading >= CLOUD_READING_INTERVAL) {
    lastCloudReading = millis();
    pushReading();
  }

  // Cloud: poll + run the next command
  if (millis() - lastCloudCommand >= CLOUD_COMMAND_INTERVAL) {
    lastCloudCommand = millis();
    pollCommands();
  }

  // Cloud: refresh schedules
  if (millis() - lastCloudSchedule >= CLOUD_SCHEDULE_INTERVAL) {
    lastCloudSchedule = millis();
    refreshSchedules();
  }

  // Scheduled auto-feeding (local clock vs HH:MM schedules)
  if (millis() - lastScheduleCheck >= SCHEDULE_CHECK_INTERVAL) {
    lastScheduleCheck = millis();
    checkScheduledFeeding();
  }

  // Ship queued log lines to the app's console. Last, so everything
  // this pass produced goes out in the same batch.
  if (millis() - lastLogFlush >= LOG_FLUSH_INTERVAL) {
    lastLogFlush = millis();
    flushLogs();
  }
}
