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
 *  Every sensor formula and calibration constant below matches the
 *  bench sketch's. The production build additionally filters pH with a
 *  median and a slow EMA so ADC noise cannot move the displayed digit.
 *  Where this firmware HAD drifted from the bench
 *  logic, it has been brought back in line:
 *
 *   1. LAST-GOOD SENSOR VALUES. The bench sketch only ever overwrites
 *      airTemperature / humidity when the DHT11 read succeeds, so a
 *      single failed read (very common on a DHT11) is invisible — it
 *      keeps showing the last good value. This firmware now does the
 *      same via a GRACE WINDOW: a sensor stays "valid" for
 *      SensorValidity::GRACE_MS after its last good read. Before, one dropped
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
 *  FEEDING (DC rotator motor) + SERVO DROP:
 *     On every feed (manual command OR schedule):
 *       1. Run the DC rotator (Motor 1) for FeedController::ROTATOR_RUN_MS (12 s)
 *          at the speed the command/schedule asked for, then stop. The app
 *          offers the five PWM levels the bench sketch tested
 *          (0/90/128/191/255); 0 skips the rotation entirely.
 *       2. After the motor stops, wait FeedController::SETTLE_MS (1.5 s).
 *       3. Servo opens to ServoConfig::OPEN_ANGLE (160 deg).
 *       4. Held open for the gram-calibrated time (5g=400ms ... 30g=2400ms).
 *       5. Servo returns to ServoConfig::CLOSED_ANGLE (180 deg).
 *
 *  TEST COMPONENTS (app Settings tab):
 *     servo_open / servo_close / motor_test drive the actuators
 *     directly. Both are bounded by the firmware, not by the app:
 *     a test-opened door force-closes after ComponentSafetyTestTimers::SERVO_MAX_OPEN_MS
 *     (10 s) and a test-driven motor stops after ComponentSafetyTestTimers::MOTOR_MAX_MS
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
#include <WebServer.h>
#include <ArduinoJson.h>
#include <time.h>
#include <DHT.h>
#include "schedule_dates.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <sys/time.h>
#include "offline_schedule_store.h"
#include "sensor_filters.h"
#include "turbidity_calibration.h"


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

// Short build tag shown in the app's Device Setup screen. Kept here rather
// than in secrets.h so it can change without touching credentials.
static const char* FIRMWARE_BUILD_TAG = "2026.11.24-sensor-calibration";

// How often the ESP32 pushes a reading to Supabase, and polls for
// commands / schedules. These are the CLOUD cadences; the LCD and the
// local serial print update on their own faster timers below.
//
// The app treats a reading as live for 3x CLOUD_READING_INTERVAL (see
// _freshWindow in lib/core/firmware_spec.dart) — change one and change
// the other.
// Publish every 3 s. Sensors are sampled every LocalSensorTiming::READ_INTERVAL_MS (2 s),
// so this keeps the app within about one sample of the LCD instead of
// the ~10 s it trailed by before.
//
// Only affordable because cloudRequest() reuses one TLS connection —
// at the old one-handshake-per-request cost, 3 s would have meant the
// device spending nearly all its time handshaking. See the shared TLS
// connection section above.
const unsigned long CLOUD_READING_INTERVAL  = 3000;   // push reading every 3 s
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
//   VERBOSE lines (the sensor block, printed every LocalSensorTiming::READ_INTERVAL_MS) are
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
  char date[11];  // "YYYY-MM-DD"; empty = legacy every-day schedule
  int  grams;
  int  rotatorPwm;
  bool active;
};

FeedSchedule schedules[MAX_SCHEDULES];
int scheduleCount = 0;
// The last schedule set Supabase sent us, kept across an offline reboot.
Preferences schedulePrefs;
// "YYYY-MM-DD HH:MM": the same wall-clock time on two dates is two feeds.
char lastFedKey[17] = "";
char lastScheduleDate[11] = "";
// A failed midnight refresh must not turn the 1 Hz schedule check into a
// stream of 10 s cloud requests. Retry at the ordinary 60 s poll cadence.
unsigned long lastScheduleDateAttempt = 0;
const unsigned long SCHEDULE_DATE_RETRY_MS = 60000;

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
// L298N MOTOR 1  ->  DC ROTATOR (runs before each feed)
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

// =====================================================
// LCD
// =====================================================
class LCDConfig {
 public:
  static const int I2C_ADDRESS = 0x27;
  static const int COLS = 16;
  static const int ROWS = 2;

  // Water/pH, housing/humidity, turbidity, ammonia, feed state.
  static const int PAGE_COUNT = 5;

  static const unsigned long UPDATE_INTERVAL_MS = 2500;
};
LiquidCrystal_I2C lcd(LCDConfig::I2C_ADDRESS, LCDConfig::COLS, LCDConfig::ROWS);

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

class ServoConfig {
 public:
  // The names lose their "SERVO_" prefix: inside ServoConfig it only
  // repeated what the scope already says.
  static const int OPEN_ANGLE   = 160;
  static const int CLOSED_ANGLE = 180;

  // Grams -> how long the door stays open. Was a free function; it is
  // servo policy, so it belongs with the angles it opens to.
  static unsigned long openMsForGrams(int grams) {
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
};



// Timings and in-flight state for one feed cycle. The actuator calls
// stay as free functions (they touch the servo, the motor and the log
// together); what is gathered here is the policy and the state those
// functions share, which was previously six loose globals.
class FeedController {
 public:
  // Pause after the rotator stops, before the feeder door opens.
  static const unsigned long SETTLE_MS = 1500;   // 1.5 s after the motor stops

  // How long the DC rotator spins before each feed. 2 s was not enough
  // to complete a turn; 10-15 s is. Anywhere in that range is fine.
  static const unsigned long ROTATOR_RUN_MS = 12000;  // 12 s
};

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

// Gram-calibrated door-open time for the feed in flight. The rotator
// deadline is serviced from loop(), so the servo phase reads it later.
unsigned long feedOpenMs = 0;

// Making the rotation non-blocking had a side effect on the ADC.
//
// crx_pulse_test.ino never samples an analog sensor while the feeder
// motor is turning — it has no feed routine at all, so motor1 only
// moves when a serial command drives it, and the sketch is otherwise
// idle. This sketch spins the rotator for FeedController::ROTATOR_RUN_MS from loop(),
// and at LocalSensorTiming::READ_INTERVAL_MS = 2 s that meant readSensors() fired about
// six times mid-spin, with a brushed DC motor loaded on the same
// supply.
//
// A motor's current draw pulls the 3.3 V rail down and couples
// switching noise into it, and all three analog sensors measure
// against that rail. Ammonia suffers worst: its whole calibrated band
// is 0.1745 V wide (AmmoniaConfig::V0 -> AmmoniaConfig::V2), so a dip of a few tens
// of millivolts is enough to throw a reading clean out of the band.
// Turbidity is next — TurbidityCalibration::V_CLEAR to TurbidityCalibration::V_HIGH spans just 0.173 V.
//
// So the analog reads are held while motor1 is energised, plus a short
// settle after it stops. The digital sensors (DS18B20, DHT11) are
// unaffected by rail noise and keep updating throughout, which also
// keeps them inside SensorValidity::GRACE_MS during a long motor test.
unsigned long adcBlockedUntil = 0;

// True when the last pass held the analog values instead of sampling.
// Only so the serial output can say so — otherwise the same turbidity
// and ammonia lines repeat during a feed and look like a frozen sensor.
bool analogHeld = false;

// A feed owns both actuators from the moment doFeed() runs until the
// servo phase calls finishFeed(). This flag stops a schedule or command
// from starting a second cycle while the first one is in flight.
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

class ComponentSafetyTestTimers {
 public:
  static const unsigned long SERVO_MAX_OPEN_MS = 10000;   // 10 s, then close
  static const unsigned long MOTOR_MAX_MS      = 30000;   // 30 s, then stop

  // 0 = this actuator was not started by a test.
  static unsigned long servoOpenedAt;
  static unsigned long motorStartedAt;

  /// True when [startedAt] is running and has passed [limitMs].
  ///
  /// Both deadlines were this same expression written out twice.
  ///
  /// The subtraction is deliberately UNSIGNED, exactly as the two call
  /// sites had it. `now - startedAt >= limitMs` on unsigned longs is
  /// what the original compared, and a signed cast -- which looks
  /// tidier -- behaves differently once millis() passes 2^31. Keeping
  /// the arithmetic identical was the point of the exercise.
  static bool expired(unsigned long startedAt, unsigned long limitMs,
                      unsigned long now) {
    return startedAt != 0 && (now - startedAt >= limitMs);
  }
};
unsigned long ComponentSafetyTestTimers::servoOpenedAt = 0;
unsigned long ComponentSafetyTestTimers::motorStartedAt = 0;


// =====================================================
// TURBIDITY CALIBRATION  (SEN0189, no divider)
// =====================================================
//
// The standard table is a five-point curve. Voltage falls as turbidity
// rises:
//
//   1.36460 V ->   0.0 %
//   1.33015 V ->  19.9 %   Clear
//   1.28065 V ->  48.5 %   Slightly Turbid
//   1.22865 V ->  78.6 %   Turbid
//   1.19160 V -> 100.0 %   Highly Turbid
//
// The three middle rows are the published voltage thresholds, so a
// reading on a threshold lands in the band named at that row. The app
// mirrors the curve and the bands in lib/core/firmware_spec.dart.

// The ADC conversion itself, which all three analog sensors wrote out
// identically.
//
// toVolts() returns DOUBLE deliberately. The original expression was
//     (analogChannels[CH_TURBIDITY].raw / 4095.0) * 3.3
// whose literals are doubles, and the result was then narrowed by the
// assignment to a float. Rewriting it with 4095.0f / 3.3f would do the
// arithmetic in float and round differently -- 714 of the 4096
// possible counts come out with different bits. The caller still
// stores a float, exactly as before.
class AdcConfig {
 public:
  static const int MAX_COUNT = 4095;

  // (double)MAX_COUNT is exactly 4095.0 -- an int this small converts
  // without rounding -- so this stays bit-identical to the literal it
  // replaces while losing the magic number.
  static double toVolts(int raw) { return (raw / (double)MAX_COUNT) * 3.3; }
};


// These bound only the rail diagnostic in printSensorData(); they do
// not clamp the curve. Both rails are still reported as 0 / 100 %, as
// the bench sketch does.


// =====================================================
// pH CALIBRATION  (3-point, explicit buffer standards)
// =====================================================
//
// The buffers are named by their standard values, not by a formula
// that assumes 7.00 / 4.00:
//
//   buffer    true pH   probe volts
//   neutral      6.86      1.950
//   acid         4.01      2.447
//   base         9.18      1.425
//
// Voltages are reconstructed probe output (pin voltage x 2 divider).
// The app records the same three constants so its reference test can
// catch drift between the two.
class PhCalibration {
 public:
  static constexpr float PH_NEUTRAL = 6.86;
  static constexpr float PH_ACID    = 4.01;
  static constexpr float PH_BASE    = 9.18;

  static constexpr float V_PH7 = 1.950;  // voltage_pH7, 6.86 buffer
  static constexpr float V_PH4 = 2.447;  // voltage_pH4, 4.01 buffer
  static constexpr float V_PH9 = 1.425;  // voltage_pH9, 9.18 buffer

  static constexpr float DIVIDER_RATIO = 2.0;

  /// Two-segment interpolation about the neutral point.
  ///
  /// The electrode's sensitivity is not symmetric: about 174 mV per pH
  /// on the acid side against 226 mV per pH on the base side. A single
  /// straight fit through all three buffers would miss the outer ones.
  static float valueOf(float probeVolts) {
    float pH;
    if (probeVolts >= V_PH7) {
      pH = PH_NEUTRAL + (probeVolts - V_PH7) * (PH_ACID - PH_NEUTRAL) /
                        (V_PH4 - V_PH7);
    } else {
      pH = PH_NEUTRAL + (probeVolts - V_PH7) * (PH_BASE - PH_NEUTRAL) /
                        (V_PH9 - V_PH7);
    }
    return constrain(pH, 0.0, 14.0);
  }
};

// One median-of-15 sample per sensor pass rejects spikes; the jump guard
// holds an unconfirmed jump and the 0.02 pH deadband stops ADC noise moving
// the last displayed digit without easing a real change over many passes.
class PhFilterConfig {
 public:
  static const int SAMPLE_COUNT = 15;
  static constexpr float MIN_CHANGE = 0.02f;
};
SensorFilters::SnapValue phFilter(PhFilterConfig::MIN_CHANGE);
SensorFilters::JumpGuard phJumpGuard;


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

class AmmoniaConfig {
 public:
  static constexpr float DIVIDER_RATIO = 2.0;

  static constexpr float V0 = 0.2655;   // volts at 0.00 mg/L
  static constexpr float V1 = 0.4030;   // volts at 0.17 mg/L
  static constexpr float V2 = 0.4400;   // volts at 0.53 mg/L

  static constexpr float C0 = 0.00;     // mg/L
  static constexpr float C1 = 0.17;
  static constexpr float C2 = 0.53;

  /// Is the reconstructed sensor voltage inside the Sera band at all?
  /// The band test was written out in readSensors(), the serial print
  /// and the LCD; now there is one of it.
  static bool inCalibrationRange(float v) { return v >= V0 && v <= V2; }

  /// Below the lowest reference, i.e. the reading has bottomed out.
  static bool belowRange(float v) { return v < V0; }

  /// Above the highest reference. The mg/L figure then reads 0.00
  /// because the curve no longer describes this sensor -- it is not a
  /// measurement, and it is not evidence that the water is clean.
  static bool aboveRange(float v) { return v > V2; }

  // The two Safe/Warning/Critical thresholds, in the class that owns
  // the calibration they describe.
  static constexpr float SAFE_MAX    = 0.25;
  static constexpr float WARNING_MAX = 0.50;

  /// const char*, not String: each of these returns one of three fixed
  /// literals, and a String would heap-allocate a copy of a literal
  /// every time the serial line or the LCD page was built -- several
  /// times a second, forever.
  static const char* statusOf(float mgL) {
    if (mgL < SAFE_MAX)    return "Safe";
    if (mgL < WARNING_MAX) return "Warning";
    return "Critical";
  }

  /// Same bands, abbreviated for the 16-column LCD.
  static const char* statusShortOf(float mgL) {
    if (mgL < SAFE_MAX)    return "Safe";
    if (mgL < WARNING_MAX) return "Warn";
    return "Crit";
  }

  /// mg/L by linear interpolation between the two Sera segments.
  /// ANYTHING outside the calibrated span reads 0.00.
  ///
  /// Both ends report C0 (0.00), deliberately:
  ///
  ///   below V0 -> 0.00. Nothing the sensor can see; the honest floor.
  ///
  ///   above V2 -> 0.00 as well. An earlier version held this at C2
  ///               (0.53) on the reasoning that past the top reference
  ///               the true concentration must be higher still. That is
  ///               sound only while the calibration matches the sensor,
  ///               and on this unit it does not: with nothing to detect
  ///               it sits around 0.629 V, nearly 0.19 V ABOVE the top
  ///               reference. So the ceiling fired constantly and the
  ///               tank read "0.53 Critical" at all times, which is both
  ///               wrong and useless as an alarm.
  ///
  /// The cost, stated plainly: while the sensor reads outside its band,
  /// a genuinely high ammonia level ALSO reports 0.00. Nothing in the
  /// firmware can distinguish the two until the MQ-137 is recalibrated
  /// against known concentrations. The serial line says when a reading
  /// is out of band and prints the voltage, which is the only
  /// trustworthy figure in that state.
  static float mgLOf(float v) {
    if (v <= V0) return C0;
    // STRICTLY above V2, matching aboveRange(). With >= the top
    // reference point itself returned 0.00, contradicting the
    // calibration table that defines 0.4400 V as C2.
    if (v >  V2) return C0;
    if (v <= V1) return C0 + (v - V0) * (C1 - C0) / (V1 - V0);
    return C1 + (v - V1) * (C2 - C1) / (V2 - V1);
  }
};


float ammoniaEstimatedMgL = 0.0;
bool  ammoniaInCalibrationRange = false;

// Turbidity raw values (globals so the printer / payload can read them).


// =====================================================
// LIVE SENSOR VALUES + per-sensor validity
// =====================================================
//
// GRACE WINDOW — matching the bench sketch's "keep the last good
// value" behaviour. A DHT11 drops reads routinely; a DS18B20 can miss
// a conversion. Flipping a sensor to invalid on the FIRST failure made
// the app blink "Not Connected" and cut the fan mid-ramp. A sensor now
// stays valid until it has been failing for this long.

// =====================================================
// THE THREE ANALOG CHANNELS, AS AN ARRAY
// =====================================================
//
// Turbidity, pH and ammonia are read the same way: analogRead the pin,
// convert the count to volts, then (pH and ammonia only) multiply back
// up through the 2:1 divider the probe sits behind. That was three
// copies of the same four lines.
//
// One descriptor per channel, one loop to sample them all. Adding a
// fourth analog sensor is now a row in this table rather than another
// copied block.
//
// Costs no RAM worth counting: three descriptors of two floats, an int
// and a const pin -- and it REPLACES the nine loose globals
// (analogChannels[CH_TURBIDITY].raw/ADCVoltage, analogChannels[CH_PH].raw/PinVoltage/Voltage,
// analogChannels[CH_AMMONIA].raw/ADCVoltage/SensorVoltage) that it now feeds.
struct AnalogChannel {
  const uint8_t pin;

  /// 1.0 for a sensor wired straight to the pin; 2.0 for one behind
  /// the 10k/10k divider, where the pin sees half the real output.
  const float dividerRatio;

  int   raw         = 0;   // 0..AdcConfig::MAX_COUNT
  float pinVolts    = 0.0; // what the ESP32 pin measures
  float sensorVolts = 0.0; // reconstructed sensor output

  void setFromRaw(int rawCount) {
    raw         = rawCount;
    pinVolts    = AdcConfig::toVolts(raw);
    sensorVolts = pinVolts * dividerRatio;
  }

  void sample() { setFromRaw(analogRead(pin)); }

  void sampleMedian(int count) {
    int samples[SensorFilters::MAX_MEDIAN_SAMPLES];
    for (int i = 0; i < count; i++) samples[i] = analogRead(pin);
    setFromRaw(SensorFilters::medianOf(samples, count));
  }
};

// Index names, so the call sites read as names and not as 0/1/2.
enum AnalogChannelId { CH_TURBIDITY = 0, CH_PH, CH_AMMONIA, CH_COUNT };

AnalogChannel analogChannels[CH_COUNT] = {
  { TURBIDITY_PIN, 1.0 },                               // no divider
  { PH_PIN,        PhCalibration::DIVIDER_RATIO },
  { AMMONIA_PIN,   AmmoniaConfig::DIVIDER_RATIO },
};


class SensorValidity {
 public:
  static const unsigned long GRACE_MS = 15000;   // 15 s of silence = really gone

  /// Has this sensor produced a reading recently enough to trust?
  ///
  /// The water-temperature and ambient paths both open with the same
  /// three-part test (ever read at all? how long since the last good
  /// one? within the grace window?). It was written out twice; this is
  /// that test, once.
  static bool isFresh(bool everRead, unsigned long lastOkAt,
                      unsigned long now) {
    return everRead && (now - lastOkAt <= GRACE_MS);
  }
};

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
// const char*, not String. statusOf() hands back a literal, so a
// String here allocated a fresh copy of that literal on every sensor
// pass and freed it on the next -- pure heap churn for a value that is
// always one of five compile-time constants.
const char* turbidityStatus = "--";
bool   turbidityValid    = false;

float pH = 7.0;
bool  phValid = false;

bool  ammoniaValid = false;

int servoPosition = 0;

int  motor1PWM = 0;        // DC rotator
int  motor2PWM = 0;        // cooling fan
bool fanActive = false;



int lcdPage = 0;
unsigned long lastLCDUpdate = 0;


// =====================================================
// LOCAL SENSOR TIMING
// =====================================================
class LocalSensorTiming {
 public:
  static const unsigned long READ_INTERVAL_MS = 2000;

  /// Settle time after the rotator stops before the ADC is trusted
  /// again. See the note on the shared supply above AnalogChannel.
  static const unsigned long ADC_SETTLE_AFTER_MOTOR_MS = 400;
};

unsigned long lastSensorRead = 0;


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

class CoolingFanConfig {
 public:
  static constexpr float START_C = 25.0;
  static constexpr float MAX_C   = 40.0;
  static const int MIN_PWM = 191;
  static const int MAX_PWM = 255;

  /// Fan PWM for a housing temperature: off below START_C, a linear
  /// ramp to MAX_C, then full. Identical to the bench sketch and to
  /// computeFanSpeed() in the app's fan_speed_util.dart.
  ///
  /// The ramp MUST stay as Arduino's map() on tenths of a degree. map()
  /// is integer arithmetic on a value truncated to a long, so it does
  /// NOT agree with the equivalent float expression: the two differ at
  /// 280 of the 1501 tenth-degree steps between 25 and 40 C. The first
  /// is 25.24 C, where map() gives 191 and the float form 192.
  ///
  /// (An earlier version of this comment claimed the split showed at
  /// 26.7 C, 197 against 198. That was wrong -- 26.7f * 10 is exactly
  /// 267.0, so both forms give 198 there. The conclusion stands, the
  /// example did not; measured, not assumed, this time.)
  ///
  /// Keeping map() keeps the fan curve bit-for-bit what it has been.
  static int pwmFor(float housingC) {
    if (housingC < START_C) return 0;
    if (housingC >= MAX_C)  return MAX_PWM;
    return map(housingC * 10, START_C * 10, MAX_C * 10, MIN_PWM, MAX_PWM);
  }
};


// =====================================================
// FORWARD DECLARATIONS
// =====================================================

bool   ackCommand(const String& id);
long   commandAgeSeconds(const String& createdAt);
void   doFeed(int grams, int rotatorPwm);
void   startRotator(unsigned long ms, int pwm);
void   serviceRotator();
void   beginServoPhase(bool afterRotation);
void   finishFeed();
void   servoTest(bool open);
void   motorTest(int pwm);
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
// =====================================================
// SHARED TLS CONNECTION TO SUPABASE
// =====================================================
// Every cloud call used to declare its own `WiFiClientSecure`, which
// meant a full TLS handshake per request — key exchange, certificate
// parsing, the lot. On an ESP32 that is roughly half a second to a
// second and a half, and it BLOCKS loop() for the whole time.
//
// In steady state the device made ~19 of those a minute (commands
// every 5 s, a reading every 10 s, schedules every 60 s), so a large
// share of every minute went on handshakes rather than work. That is
// what made readings arrive late: pushReading() could not get its slot
// on time, and the app saw gaps far longer than the publish interval.
//
// One client, kept open and reused, turns a request into just the POST
// or GET on an established socket — a few hundred milliseconds at
// most. That is what makes a 3 s publish interval affordable; raising
// the rate without this would only have deepened the congestion.
//
// The connection is dropped and re-made on transport failure, because
// the far end may close a socket we still believe is good.
WiFiClientSecure cloudClient;
bool cloudTlsConfigured = false;

const unsigned long CLOUD_HTTP_TIMEOUT_MS = 10000;

/// Request failed before any HTTP status came back.
const int CLOUD_ERR_BEGIN = -1000;

void cloudDropConnection() {
  cloudClient.stop();
}

/// One Supabase request over the shared connection.
///
/// [method] is "GET", "POST" or "PATCH". [body] is sent for anything
/// other than GET. When [out] is given the response body is read into
/// it. [minimal] adds `Prefer: return=minimal`, which stops PostgREST
/// sending the inserted row back — less for the device to read, and a
/// faster round trip.
///
/// Retries once on a transport-level failure (code < 0), since the
/// most likely cause is a reused socket the server has since closed.
int cloudRequest(const String& url, const char* method, const String& body,
                 String* out = nullptr, bool minimal = false) {
  if (WiFi.status() != WL_CONNECTED) return CLOUD_ERR_BEGIN;

  if (!cloudTlsConfigured) {
    // thesis simplicity; pin the CA cert for production-grade TLS
    cloudClient.setInsecure();
    cloudTlsConfigured = true;
  }

  for (int attempt = 0; attempt < 2; attempt++) {
    HTTPClient http;
    http.setReuse(true);          // keep the socket open after end()
    if (!http.begin(cloudClient, url)) return CLOUD_ERR_BEGIN;

    http.addHeader("apikey", SUPABASE_KEY);
    http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
    if (body.length()) http.addHeader("Content-Type", "application/json");
    if (minimal)       http.addHeader("Prefer", "return=minimal");
    http.setTimeout(CLOUD_HTTP_TIMEOUT_MS);

    int code;
    if      (strcmp(method, "GET")  == 0) code = http.GET();
    else if (strcmp(method, "POST") == 0) code = http.POST(body);
    else                                  code = http.sendRequest(method, body);

    if (code < 0 && attempt == 0) {
      // Transport failure on a connection we were reusing: drop it and
      // handshake once more before giving up.
      http.end();
      cloudDropConnection();
      continue;
    }

    if (out != nullptr && code > 0) *out = http.getString();
    http.end();                   // keeps the socket open (setReuse)
    return code;
  }

  return CLOUD_ERR_BEGIN;
}

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

  const int code = cloudRequest(
      String(SUPABASE_URL) + "/rest/v1/device_logs", "POST", body, nullptr, true);
  if (!(code == 200 || code == 201 || code == 204)) {
    // Serial only — logging a failed log upload through logLine() would
    // queue a line that the next flush tries to send, and so on.
    Serial.print("[Cloud] device_logs insert -> HTTP ");
    Serial.println(code);
  }
}


// =====================================================
// WIFI
// =====================================================

// ---- Why a join failed ----
//
// This used to print "WiFi: retrying..." forever and nothing else, so
// a failure to connect looked identical whatever caused it: wrong
// password, 5 GHz-only SSID, router full, out of range. The radio knows
// which it was -- the ESP32 reports a numeric reason on every
// disconnect -- it just was not being read.
volatile uint8_t wifiLastReason = 0;
bool wifiHaveReason = false;
int  wifiFailedAttempts = 0;

/// Set around our own WiFi.disconnect() calls.
///
/// Without it the handler below records reason 36 (STA_LEAVING) from the
/// retry's own disconnect and overwrites whatever the radio actually
/// reported, so every failure looks like "we left" no matter what went
/// wrong. The first genuine reason is the useful one, so it is kept.
bool wifiSelfDisconnect = false;

const char* wifiStatusName(int s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "idle";
    case WL_NO_SSID_AVAIL:   return "SSID not found on 2.4 GHz";
    case WL_SCAN_COMPLETED:  return "scan done";
    case WL_CONNECTED:       return "connected";
    case WL_CONNECT_FAILED:  return "connect failed";
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_DISCONNECTED:    return "disconnected";
    default:                 return "unknown";
  }
}

/// The disconnect reason, in terms of what to actually go and change.
const char* wifiReasonHint(uint8_t r) {
  switch (r) {
    case 201: return "NO_AP_FOUND - the radio cannot see that SSID at all. "
                     "Most likely the hotspot is on 5 GHz (the ESP32 is "
                     "2.4 GHz ONLY), or the name has a typo/extra space, or "
                     "it is out of range, or the SSID is hidden.";
    case 15:  return "4WAY_HANDSHAKE_TIMEOUT - the AP was found and refused "
                     "the key. Wrong password, or the hotspot is WPA3-only "
                     "(set it to WPA2-PSK / WPA2-Personal).";
    case 2:   return "AUTH_EXPIRE - authentication timed out. Weak signal, "
                     "or WPA3/enterprise security.";
    case 202: return "AUTH_FAIL - the AP rejected authentication. Usually "
                     "the password, sometimes WPA3-only or MAC filtering.";
    case 5:   return "ASSOC_TOOMANY - the hotspot has hit its client limit. "
                     "Pocket WiFi units often cap at 8-10 devices; "
                     "disconnect something else and retry.";
    case 6:
    case 7:   return "NOT_AUTHED / NOT_ASSOCED - the AP dropped us before "
                     "the key exchange. Often MAC filtering.";
    case 23:  return "802_1X_AUTH_FAILED - this is an enterprise network. "
                     "The firmware only does WPA2-PSK.";
    case 200: return "BEACON_TIMEOUT - was associated, then stopped hearing "
                     "the AP. Signal or the hotspot slept.";
    case 203: return "ASSOC_FAIL - association refused by the AP.";
    case 204: return "HANDSHAKE_TIMEOUT - key exchange stalled.";
    case 205: return "CONNECTION_FAIL - generic failure; check the band and "
                     "the password first.";
    case 3:
    case 8:
    case 36:  return "STA_LEAVING / ASSOC_LEAVE - this board ended the "
                     "association itself, which is what the retry below "
                     "does. It masks the real reason rather than being one; "
                     "read the reason printed on attempt 1.";
    case 39:  return "TIMEOUT - generic timeout during the join.";
    default:  return "see esp_wifi_types_generic.h WIFI_REASON_* for this code";
  }
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) return;
  if (wifiSelfDisconnect) return;          // our own doing, says nothing
  wifiLastReason = info.wifi_sta_disconnected.reason;
  wifiHaveReason = true;
}

/// Scan and say whether the configured SSID is actually visible.
///
/// The ESP32 radio is 2.4 GHz only, so "not in this list" is itself the
/// answer most of the time: a dual-band hotspot advertising one name on
/// 5 GHz is invisible here and looks exactly like a wrong password.
///
/// Blocking for a couple of seconds, which is fine -- it only runs
/// while there is no WiFi, and nothing else can make progress then.
void reportVisibleNetworks() {
  Serial.println();
  Serial.println("--- 2.4 GHz networks this board can see ---");

  // The radio must be ON and IDLE. Scanning while a join is in flight
  // returns 0 even with networks all around, which is exactly what made
  // the first version of this report "(none)" and point the finger at
  // the band for no reason. So: stop any attempt, let it settle, scan.
  wifiSelfDisconnect = true;
  WiFi.disconnect(false, false);     // keep the radio powered
  delay(200);
  wifiSelfDisconnect = false;

  // show_hidden = true: a hotspot with a hidden SSID is otherwise
  // invisible here and looks like the wrong band.
  const int n = WiFi.scanNetworks(false, true);

  if (n < 0) {
    Serial.print("  scan failed (");
    Serial.print(n);
    Serial.println(") - the radio was busy or is not initialised.");
    Serial.println("  This is a board-side fault, NOT a hotspot problem.");
    WiFi.scanDelete();
    return;
  }
  if (n == 0) {
    Serial.println("  (none) - zero 2.4 GHz APs, not even neighbours'.");
    Serial.println("  In any populated area that means the RADIO is the");
    Serial.println("  problem, not the hotspot. Usual causes:");
    Serial.println("   - under-powered USB supply: the WiFi TX spike (~300 mA)");
    Serial.println("     browns out the radio. Try a different cable/port, or");
    Serial.println("     a powered hub, and avoid driving the motors while");
    Serial.println("     joining.");
    Serial.println("   - a damaged or unconnected antenna on the module.");
    WiFi.scanDelete();
    return;
  }

  bool found = false;
  for (int i = 0; i < n; i++) {
    const String seen = WiFi.SSID(i);
    const bool mine = seen.equals(ssid);
    if (mine) found = true;

    Serial.print(mine ? "  >> " : "     ");
    Serial.print(seen.length() ? seen : String("(hidden)"));
    Serial.print("   ");
    Serial.print(WiFi.RSSI(i));
    Serial.print(" dBm   ch ");
    Serial.print(WiFi.channel(i));
    Serial.print("   ");
    switch (WiFi.encryptionType(i)) {
      case WIFI_AUTH_OPEN:            Serial.print("OPEN"); break;
      case WIFI_AUTH_WEP:             Serial.print("WEP"); break;
      case WIFI_AUTH_WPA_PSK:         Serial.print("WPA"); break;
      case WIFI_AUTH_WPA2_PSK:        Serial.print("WPA2"); break;
      case WIFI_AUTH_WPA_WPA2_PSK:    Serial.print("WPA/WPA2"); break;
      case WIFI_AUTH_WPA3_PSK:        Serial.print("WPA3 (ESP32 may refuse)"); break;
      case WIFI_AUTH_WPA2_WPA3_PSK:   Serial.print("WPA2/WPA3"); break;
      case WIFI_AUTH_WPA2_ENTERPRISE: Serial.print("ENTERPRISE (unsupported)"); break;
      default:                        Serial.print("other"); break;
    }
    Serial.println(mine ? "   <= the configured SSID" : "");
  }
  WiFi.scanDelete();

  Serial.println();
  if (found) {
    Serial.println("  The SSID IS visible, so the name and the band are fine.");
    Serial.println("  That points at the password, or at WPA3 / MAC filtering /");
    Serial.println("  the hotspot's client limit. See the reason code above.");
  } else {
    Serial.println("  The configured SSID is NOT in that list.");
    Serial.println("  Check, in this order:");
    Serial.println("   1. the hotspot's 2.4 GHz radio is ON (ESP32 cannot do 5 GHz)");
    Serial.println("   2. the SSID in secrets.h matches EXACTLY - case, spaces,");
    Serial.println("      punctuation. A trailing space is invisible and fatal.");
    Serial.println("   3. secrets.h was saved AND the sketch re-uploaded. The");
    Serial.println("      credentials are compiled in, so editing the file alone");
    Serial.println("      changes nothing on the board.");
    Serial.println("   4. the board is within a few metres of the hotspot.");
  }
  Serial.println("-------------------------------------------");
}

// =====================================================
// OFFLINE ACCESS POINT + LOCAL READ-ONLY API
// =====================================================
//
// Purpose: when the tank's WiFi is not available, the device raises its
// own access point so a phone can join it directly and keep watching
// readings. The phone stores what it pulls and uploads it to Supabase
// later, once it has internet again.
//
// Everything here is ADDITIVE. It reads the same globals readSensors()
// already fills and writes none of them, so the sensor pipeline, the
// calibration, the LCD and the 3 s cloud push behave exactly as before.
// If the AP is never raised, not one line of this runs.
//
// The real AP password lives in secrets.h, which is gitignored --
// secrets.h is included above, so its #define wins over the fallback
// here. This fallback only exists so the sketch still builds for
// someone who has not set one, and is deliberately not a password
// anybody should ship: WPA2 needs 8-63 characters, and this is a
// placeholder, not a secret.
#ifndef AP_PASSWORD
#define AP_PASSWORD "CHANGE_ME_IN_SECRETS_H"
#endif

// Raise the AP once the station has been down this long. Deliberately
// short: the point is that somebody standing at the tank does not wait.
const unsigned long AP_FALLBACK_AFTER_MS = 5000;

// Once WiFi is back, keep the AP up this long after the last client
// leaves, so a phone mid-session is not dropped the moment the router
// reappears.
const unsigned long AP_LINGER_AFTER_STA_MS = 60000;

WebServer apServer(80);

bool apActive = false;
bool apServerStarted = false;
unsigned long staDownSince = 0;     // 0 = station is up (or never tried)
unsigned long apClientsLastSeen = 0;

// Last client count we announced, so a join or a leave is logged once
// instead of on every loop pass.
unsigned int apClientsLast = 0;

/// The current reading, in exactly the field names a `readings` row
/// uses.
///
/// Same names on purpose: the app hands this straight to the function
/// that already parses a Supabase row, so every freshness rule and
/// per-sensor validity gate it applies to cloud data applies here too,
/// with no second code path to keep in step.
String localReadingJson() {
  StaticJsonDocument<512> doc;
  doc["device_id"]        = DEVICE_ID;
  doc["ph"]               = serialized(String(pH, 2));
  doc["ammonia"]          = serialized(String(ammoniaEstimatedMgL, 2));
  doc["turbidity"]        = serialized(String(relativeTurbidity, 1));
  doc["turbidity_status"] = turbidityStatus;
  doc["water_temp_c"]     = serialized(String(waterTemperature, 1));
  doc["ambient_temp_c"]   = serialized(String(airTemperature, 1));
  doc["fan_active"]       = fanActive;
  doc["fan_percent"]      = (int)((motor2PWM / 255.0) * 100.0);
  doc["ph_valid"]         = phValid;
  doc["turbidity_valid"]  = turbidityValid;
  doc["water_temp_valid"] = waterTempValid;
  doc["ambient_valid"]    = ambientValid;
  doc["ammonia_valid"]    = ammoniaValid;

  // No recorded_at. The device has no trustworthy clock while it is
  // offline -- NTP needs internet -- so the PHONE stamps each reading
  // with its own clock as it stores it. Sending a made-up time here
  // would be worse than sending none.
  String out;
  serializeJson(doc, out);
  return out;
}

void handleApiLive() {
  apServer.sendHeader("Access-Control-Allow-Origin", "*");
  apServer.send(200, "application/json", localReadingJson());
}

void handleApiStatus() {
  StaticJsonDocument<384> doc;
  doc["device_id"]   = DEVICE_ID;
  doc["firmware"]    = FIRMWARE_VERSION;
  doc["build_tag"]   = FIRMWARE_BUILD_TAG;
  doc["sta_connected"] = (WiFi.status() == WL_CONNECTED);
  doc["ap_active"]   = apActive;
  doc["ap_clients"]  = WiFi.softAPgetStationNum();
  doc["uptime_s"]    = (unsigned long)(millis() / 1000);
  // So the app can tell "the device is fine, it just has no WiFi" from
  // "the device is in trouble".
  doc["free_heap"]   = ESP.getFreeHeap();

  String out;
  serializeJson(doc, out);
  apServer.sendHeader("Access-Control-Allow-Origin", "*");
  apServer.send(200, "application/json", out);
}

// ---- Offline control surface on the access point --------------------
//
// The AP used to serve readings only. These routes let the app do the same
// jobs it does through Supabase when the tank has no internet at all.
// Every POST carries device_id so a stranger on 192.168.4.1 cannot drive
// the actuators.

void sendApReply(int code, const String& payload) {
  apServer.sendHeader("Access-Control-Allow-Origin", "*");
  apServer.send(code, "application/json", payload);
}

/// Every accepted write returns the board's id as well as `ok`, so the app
/// can tell the tank's acknowledgement from a stranger's 200 response.
void sendApOk() {
  StaticJsonDocument<128> doc;
  doc["ok"] = true;
  doc["device_id"] = DEVICE_ID;
  String out;
  serializeJson(doc, out);
  sendApReply(200, out);
}

void handleApiTime() {
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, apServer.arg("plain")) ||
      doc["device_id"].as<String>() != String(DEVICE_ID)) {
    sendApReply(400, "{\"error\":\"bad request\"}");
    return;
  }
  const long long epoch = doc["epoch_seconds"].as<long long>();
  if (epoch <= 0) {
    sendApReply(400, "{\"error\":\"bad epoch\"}");
    return;
  }
  // configTime() still supplies the UTC+8 offset; the phone only seeds the
  // UTC epoch, which is all an offline board can know about the time.
  struct timeval tv;
  tv.tv_sec = (time_t)epoch;
  tv.tv_usec = 0;
  // The app only marks the seed done on a 2xx reply. A firmware-side
  // failure must not be reported as success, or dated feeds stay silent
  // while both sides believe the clock was set.
  if (settimeofday(&tv, nullptr) != 0) {
    sendApReply(500, "{\"error\":\"clock rejected\"}");
    return;
  }
  timeSynced = true;
  sendApOk();
}

void handleApiFeed() {
  StaticJsonDocument<192> doc;
  if (deserializeJson(doc, apServer.arg("plain")) ||
      doc["device_id"].as<String>() != String(DEVICE_ID)) {
    sendApReply(400, "{\"error\":\"bad request\"}");
    return;
  }
  const int grams = doc["grams"] | 10;
  const int pwm = doc["rotator_pwm"] | 255;
  logLine("[AP] feed_now " + String(grams) + " g");
  doFeed(grams, pwm);
  sendApOk();
}

void handleApiCommand() {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, apServer.arg("plain")) ||
      doc["device_id"].as<String>() != String(DEVICE_ID)) {
    sendApReply(400, "{\"error\":\"bad request\"}");
    return;
  }

  const String type = doc["type"].as<String>();
  if (type == "feed_now") {
    doFeed(doc["grams"] | 10, doc["rotator_pwm"] | 255);
  } else if (type == "servo_open") {
    servoTest(true);
  } else if (type == "servo_close") {
    servoTest(false);
  } else if (type == "motor_test") {
    motorTest(doc["value"] | 0);
  } else if (type == "log_stream") {
    long secs = doc["value"] | 0;
    if (secs <= 0) {
      logStreamUntil = 0;
    } else {
      if (secs > (long)LOG_STREAM_MAX_S) secs = LOG_STREAM_MAX_S;
      logStreamUntil = millis() + (unsigned long)secs * 1000UL;
      if (logStreamUntil == 0) logStreamUntil = 1;
    }
  } else {
    sendApReply(400, "{\"error\":\"unknown command\"}");
    return;
  }
  sendApOk();
}

/// Lets the app show the board's cached feeds while offline. Indices are
/// part of the reply because every edit/delete is addressed by index.
void handleApiSchedulesList() {
  StaticJsonDocument<3072> doc;
  JsonArray rows = doc.createNestedArray("schedules");
  for (int i = 0; i < scheduleCount; i++) {
    JsonObject row = rows.createNestedObject();
    row["index"] = i;
    row["time"] = schedules[i].time;
    // An empty date is the legacy every-day row; send JSON null for it.
    if (schedules[i].date[0] != '\0') row["feed_date"] = schedules[i].date;
    row["grams"] = schedules[i].grams;
    row["rotator_pwm"] = schedules[i].rotatorPwm;
    row["active"] = schedules[i].active;
    row["device_id"] = DEVICE_ID;
  }
  String out;
  serializeJson(doc, out);
  sendApReply(200, out);
}

void handleApiSchedules() {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, apServer.arg("plain")) ||
      doc["device_id"].as<String>() != String(DEVICE_ID)) {
    sendApReply(400, "{\"error\":\"bad request\"}");
    return;
  }

  // An index overwrites one slot (a toggle or an edit); no index appends.
  const int index = doc["index"] | -1;
  if (index >= scheduleCount) {
    sendApReply(400, "{\"error\":\"bad index\"}");
    return;
  }
  if (index < 0 && scheduleCount >= MAX_SCHEDULES) {
    sendApReply(409, "{\"error\":\"schedule cache full\"}");
    return;
  }

  // Validate every field before touching a slot: a bad date must not leave
  // a half-written row behind when the request is rejected.
  const String t = doc["time"].as<String>();
  if (!isValidScheduleTime(t.c_str())) {
    sendApReply(400, "{\"error\":\"bad time\"}");
    return;
  }
  char nextDate[11] = "";
  if (!doc["feed_date"].isNull()) {
    const String d = doc["feed_date"].as<String>();
    if (!isValidScheduleDate(d.c_str())) {
      sendApReply(400, "{\"error\":\"bad date\"}");
      return;
    }
    strncpy(nextDate, d.c_str(), sizeof(nextDate) - 1);
  }

  FeedSchedule& row = index >= 0 ? schedules[index] : schedules[scheduleCount];
  strncpy(row.time, t.c_str(), 5);
  row.time[5] = '\0';
  strncpy(row.date, nextDate, sizeof(row.date) - 1);
  row.date[sizeof(row.date) - 1] = '\0';
  row.grams = doc["grams"] | 10;
  row.rotatorPwm = doc["rotator_pwm"] | 255;
  row.active = doc["active"] | true;
  if (index < 0) scheduleCount++;
  persistSchedules();
  sendApOk();
}

void handleApiSchedulesDelete() {
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, apServer.arg("plain")) ||
      doc["device_id"].as<String>() != String(DEVICE_ID)) {
    sendApReply(400, "{\"error\":\"bad request\"}");
    return;
  }
  // A missing index must not silently remove slot 0. Match the add/update
  // handler's -1 default and reject anything outside the cache.
  const int index = doc["index"] | -1;
  if (index < 0 || index >= scheduleCount) {
    sendApReply(400, "{\"error\":\"bad index\"}");
    return;
  }
  for (int i = index; i < scheduleCount - 1; i++) {
    schedules[i] = schedules[i + 1];
  }
  scheduleCount--;
  persistSchedules();
  sendApOk();
}

/// AP SSID, unique per board so two units never collide and the app can
/// recognise one by prefix.
String apSsid() {
  String mac = WiFi.macAddress();      // AA:BB:CC:DD:EE:FF
  mac.replace(":", "");
  return "CRX-Pulse-" + mac.substring(6);
}

void startAccessPoint() {
  if (apActive) return;

  // AP_STA, not AP: the station must keep retrying the real WiFi while
  // the AP serves, or the device could never come back online on its
  // own.
  WiFi.mode(WIFI_AP_STA);
  const String ssid_ap = apSsid();

  if (!WiFi.softAP(ssid_ap.c_str(), AP_PASSWORD)) {
    Serial.println("[AP] softAP() failed");
    return;
  }
  apActive = true;
  apClientsLastSeen = millis();

  if (!apServerStarted) {
    apServer.on("/api/live",   HTTP_GET, handleApiLive);
    apServer.on("/api/status", HTTP_GET, handleApiStatus);
    apServer.on("/api/schedules", HTTP_GET, handleApiSchedulesList);
    apServer.on("/api/time",   HTTP_POST, handleApiTime);
    apServer.on("/api/feed",   HTTP_POST, handleApiFeed);
    apServer.on("/api/command", HTTP_POST, handleApiCommand);
    apServer.on("/api/schedules", HTTP_POST, handleApiSchedules);
    apServer.on("/api/schedules/delete", HTTP_POST, handleApiSchedulesDelete);
    apServer.onNotFound([]() {
      apServer.send(404, "application/json", "{\"error\":\"no such route\"}");
    });
    apServer.begin();
    apServerStarted = true;
  }

  Serial.println();
  Serial.println("================================");
  Serial.println("OFFLINE ACCESS POINT UP");
  Serial.print("  SSID:     "); Serial.println(ssid_ap);
  Serial.print("  Address:  http://"); Serial.println(WiFi.softAPIP());
  Serial.println("  Join it from the phone, then open the app.");
  Serial.println("================================");
}

void stopAccessPoint() {
  if (!apActive) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActive = false;
  Serial.println("[AP] offline access point down - WiFi is back");
}

/// Called every loop pass. Raises the AP when the station has been down
/// for AP_FALLBACK_AFTER_MS, and takes it away again once WiFi is back
/// and nobody is using it.
void serviceAccessPoint() {
  const unsigned long now = millis();
  const bool staUp = (WiFi.status() == WL_CONNECTED);

  // Hoisted out of the branch below so the LED and the log can see it
  // too. softAPgetStationNum() has no side effects and reads 0 while the
  // AP is down, so asking here rather than inside the else branch
  // leaves every decision underneath exactly as it was.
  const unsigned int apClients = apActive ? WiFi.softAPgetStationNum() : 0;

  if (staUp) {
    staDownSince = 0;
  } else if (staDownSince == 0) {
    staDownSince = now;                       // start the 5 s clock
  }

  if (!apActive) {
    if (!staUp && (now - staDownSince >= AP_FALLBACK_AFTER_MS)) {
      startAccessPoint();
    }
  } else {
    if (apClients > 0) {
      apClientsLastSeen = now;                // somebody is attached
    }
    // Only ever withdraw it when the real WiFi can take over AND the
    // phone has had time to notice.
    if (staUp && (now - apClientsLastSeen >= AP_LINGER_AFTER_STA_MS)) {
      stopAccessPoint();
    }
  }

  // ---- WiFi LED ------------------------------------------------------
  // The LED's meaning widens from "the tank WiFi is up" to "this device
  // has a link that can carry its readings": the tank WiFi, or a phone
  // joined to the offline access point.
  //
  // checkWiFi() has already driven this pin from the station's state by
  // the time we get here -- loop() calls serviceAccessPoint() directly
  // after it -- so writing HIGH here only ever ADDS a reason for the LED
  // to be lit. checkWiFi() itself is untouched, and when neither link
  // exists the pin simply keeps the LOW it just wrote.
  if (apClients > 0) digitalWrite(WIFI_LED_PIN, HIGH);

  if (apClients != apClientsLast) {
    if (apClientsLast == 0) {
      Serial.print("[AP] phone joined - LED on, ");
      Serial.print(apClients);
      Serial.println(" client(s). Open the app to read the tank.");
    } else if (apClients == 0) {
      Serial.println("[AP] last client left - LED follows the tank WiFi again");
    } else {
      Serial.print("[AP] clients now ");
      Serial.println(apClients);
    }
    apClientsLast = apClients;
  }

  if (apActive) apServer.handleClient();
}

void startWiFi() {
  Serial.println();
  Serial.println("================================");
  Serial.println("WIFI CONNECTION");
  Serial.println("================================");
  Serial.print("SSID: ");
  Serial.println(ssid);

  // Full radio reset, OFF then STA.
  //
  // NOT WiFi.disconnect(true, ...) -- that first argument is `wifioff`,
  // so it powers the radio DOWN, and anything done afterwards (a scan,
  // in particular) then runs against a dead radio and reports nothing.
  // Cycling the mode leaves the radio on and initialised, and still
  // clears any stale association carried over from another network.
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  delay(150);
  WiFi.mode(WIFI_STA);
  delay(150);

  // Sleep off while joining. The modem power-save that is on by default
  // makes association flaky on hotspots with short beacon intervals,
  // which pocket WiFi units often have.
  WiFi.setSleep(false);

  // So the failure reason is available when it fails.
  WiFi.onEvent(onWiFiEvent);
  wifiHaveReason = false;
  wifiSelfDisconnect = false;
  wifiFailedAttempts = 0;

  digitalWrite(WIFI_LED_PIN, LOW);

  // The MAC, for checking the hotspot's client list or a MAC filter.
  Serial.print("This board's MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("Free heap: ");
  Serial.print(ESP.getFreeHeap());
  Serial.println(" bytes");
  Serial.println("Band: 2.4 GHz only - the ESP32 has no 5 GHz radio.");

  // Scan BEFORE the first join, while the radio is idle. This is the
  // only moment the scan is trustworthy; once begin() is in flight it
  // returns 0 regardless of what is in range.
  reportVisibleNetworks();

  WiFi.setAutoReconnect(true);
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
    wifiFailedAttempts++;

    Serial.println();
    Serial.print("WiFi: attempt ");
    Serial.print(wifiFailedAttempts);
    Serial.print(" failed - status: ");
    Serial.println(wifiStatusName(WiFi.status()));

    if (wifiHaveReason) {
      Serial.print("  reason ");
      Serial.print(wifiLastReason);
      Serial.print(": ");
      Serial.println(wifiReasonHint(wifiLastReason));
    }

    // Re-scan occasionally, not every time: it blocks for a couple of
    // seconds and the answer rarely changes. startWiFi() already
    // scanned once on a clean radio, which is the reliable one.
    if (wifiFailedAttempts % 10 == 0) {
      reportVisibleNetworks();
    }

    Serial.println("WiFi: retrying...");
    wifiSelfDisconnect = true;
    WiFi.disconnect(false, false);   // leave the radio powered
    delay(150);
    wifiSelfDisconnect = false;
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
// described at SensorValidity::GRACE_MS so a single dropped read is not treated
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
  waterTempValid =
      SensorValidity::isFresh(waterTempEverRead, lastWaterTempOk, now);

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
  ambientValid = SensorValidity::isFresh(ambientEverRead, lastAmbientOk, now);

  // ---- Analog sensors: only while the rotator is quiet ----
  //
  // See LocalSensorTiming::ADC_SETTLE_AFTER_MOTOR_MS. Holding the previous values beats
  // publishing ones taken through motor noise — a held reading is
  // merely a couple of seconds stale, whereas a noisy one is wrong and
  // indistinguishable from a real measurement.
  if ((long)(now - adcBlockedUntil) < 0) {
    analogHeld = true;
    return;
  }
  analogHeld = false;

  // ---- The three analog channels, sampled through one pipeline ----
  //
  // These were three copied blocks that each did analogRead, then the
  // same (raw / 4095.0) * 3.3 conversion, then an optional divider
  // multiply -- the same shape three times, with the conversion
  // constants written out alongside it three times.
  //
  // Now they are an array walked by one loop. pH is the one exception
  // in how they are sampled: its high-impedance signal takes a median
  // of 15 reads before the same voltage conversion.
  for (int i = 0; i < CH_COUNT; i++) {
    if (i == CH_PH) {
      analogChannels[i].sampleMedian(PhFilterConfig::SAMPLE_COUNT);
    } else {
      analogChannels[i].sample();
    }
  }

  const AnalogChannel& turb = analogChannels[CH_TURBIDITY];
  const AnalogChannel& phCh = analogChannels[CH_PH];
  const AnalogChannel& nh3  = analogChannels[CH_AMMONIA];

  // ---------------- Turbidity (SEN0189) ---------------

  // Bench parity: crx_pulse_test.ino has no validity concept for
  // turbidity at all - it converts whatever the ADC gives and always
  // reports a percentage. This sketch previously rejected readings at
  // either rail as implausible, which is why the app and the LCD showed
  // "Turbidity --" instead of a number.
  //
  // Keeping the flag (the `readings` table and the app both read it) but
  // always true, so the published behaviour matches the bench.
  //
  // The cost, stated plainly: a disconnected probe reads ~0 V, and 0 V
  // converts to a confident "100.0% Highly Turbid" that is
  // indistinguishable from a real measurement. That is the bench's
  // behaviour too - it is not a safety check that was lost, it is one
  // the bench never had.
  turbidityValid = true;

  relativeTurbidity = TurbidityCalibration::percentOf(turb.pinVolts);
  turbidityStatus   = TurbidityCalibration::statusOf(relativeTurbidity);

  // ---------------- pH (3-point) ----------------------
  // Bench parity, as for turbidity above: crx_pulse_test.ino converts
  // and reports unconditionally, with no rail check. The median, jump
  // guard and snap publish live after calibration, so filtering cannot
  // shift the buffer mapping.
  phValid = true;
  pH = phFilter.update(phJumpGuard.update(PhCalibration::valueOf(phCh.sensorVolts)));

  // ---------------- MQ-137 ammonia --------------------

  // NH4/NH3 estimate (mg/L) - crx_pulse_test.ino's logic. Computed
  // ONLY inside the Sera band; outside it the bench sketch does not
  // clamp to an end of the curve, and neither does this.
  //
  // That clamp is where "0.53 mg/L (Critical)" came from: 0.53 is
  // simply C2, the top reference point, reported as though the sensor
  // had produced it while it actually sat 0.19 V beyond.
  ammoniaInCalibrationRange =
      AmmoniaConfig::inCalibrationRange(analogChannels[CH_AMMONIA].sensorVolts);
  ammoniaEstimatedMgL = AmmoniaConfig::mgLOf(analogChannels[CH_AMMONIA].sensorVolts);

  // Always valid now: mgLOf() clamps instead of refusing, so there is
  // always a figure to publish. The app showed "Not Connected" for any
  // reading outside the Sera band, which is what this removes.
  ammoniaValid = true;
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
  // Only a genuinely dead DHT11 holds the fan off; the curve itself is
  // CoolingFanConfig's business.
  pwm = ambientValid ? CoolingFanConfig::pwmFor(airTemperature) : 0;

  pwm = constrain(pwm, 0, 255);
  motor2PWM = pwm;
  fanActive = (pwm > 0);
  analogWrite(MOTOR2_EN, motor2PWM);
}


// =====================================================
// DC ROTATOR (Motor 1) — runs before each feed
// =====================================================

/// Run the rotator at [pwm] for [ms] and return immediately. [pwm] comes
/// from the app's rotator-speed selection; 0 means "do not rotate at all",
/// which is a legitimate choice for a feed.
/// This is the first phase of the feed. serviceRotator() stops the motor
/// and hands over to beginServoPhase(), so feedInProgress stays set until
/// the servo has dispensed.
void startRotator(unsigned long ms, int pwm) {
  pwm = constrain(pwm, 0, 255);

  if (pwm == 0 || ms == 0) {
    logLine("[Feed] Rotator -> skipped (speed set to Off)");
    beginServoPhase(false);
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

/// Stop the rotator once its deadline passes, then start the servo phase.
/// Called every loop pass.
void serviceRotator() {
  if (rotatorStopAt == 0) return;
  if ((long)(millis() - rotatorStopAt) < 0) return;

  rotatorStopAt = 0;
  motor1PWM = 0;
  analogWrite(MOTOR1_EN, motor1PWM);
  logLine("[Feed] Rotator -> OFF");
  beginServoPhase(true);
}

/// Second phase of a feed: dispense from the servo, then release the lock.
/// [afterRotation] keeps the settle pause only when the motor actually ran.
void beginServoPhase(bool afterRotation) {
  if (afterRotation) {
    logLine("[Feed] Settling 1.5 s before dispensing...");
    lcd.setCursor(0, 1);
    lcd.print("Settling...  ");
    delay(FeedController::SETTLE_MS);
  }

  lcd.setCursor(0, 1);
  lcd.print("Dispensing...");
  myServo.write(ServoConfig::OPEN_ANGLE);
  servoPosition = ServoConfig::OPEN_ANGLE;
  delay(feedOpenMs);
  myServo.write(ServoConfig::CLOSED_ANGLE);
  servoPosition = ServoConfig::CLOSED_ANGLE;

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
    myServo.write(ServoConfig::OPEN_ANGLE);
    servoPosition = ServoConfig::OPEN_ANGLE;
    ComponentSafetyTestTimers::servoOpenedAt = millis();
    logLine("[Test] Servo -> OPEN " + String(ServoConfig::OPEN_ANGLE) + " degrees");
  } else {
    myServo.write(ServoConfig::CLOSED_ANGLE);
    servoPosition = ServoConfig::CLOSED_ANGLE;
    ComponentSafetyTestTimers::servoOpenedAt = 0;
    logLine("[Test] Servo -> CLOSED " + String(ServoConfig::CLOSED_ANGLE) + " degrees");
  }
}

void motorTest(int pwm) {
  if (feedInProgress) {
    logLine("[Test] Motor ignored — a feed is running");
    return;
  }

  motor1PWM = constrain(pwm, 0, 255);
  analogWrite(MOTOR1_EN, motor1PWM);
  ComponentSafetyTestTimers::motorStartedAt = (motor1PWM > 0) ? millis() : 0;

  logLine("[Test] Rotator -> " + String(motor1PWM) + "/255 (" +
          String((int)((motor1PWM / 255.0) * 100)) + "%)");
}

/// Enforce the deadlines above. Called every loop pass.
void checkTestTimeouts() {
  const unsigned long now = millis();

  if (ComponentSafetyTestTimers::expired(
          ComponentSafetyTestTimers::servoOpenedAt,
          ComponentSafetyTestTimers::SERVO_MAX_OPEN_MS, now)) {
    myServo.write(ServoConfig::CLOSED_ANGLE);
    servoPosition = ServoConfig::CLOSED_ANGLE;
    ComponentSafetyTestTimers::servoOpenedAt = 0;
    logLine("[Test] Servo auto-closed after 10 s");
  }

  if (ComponentSafetyTestTimers::expired(
          ComponentSafetyTestTimers::motorStartedAt,
          ComponentSafetyTestTimers::MOTOR_MAX_MS, now)) {
    motor1PWM = 0;
    analogWrite(MOTOR1_EN, motor1PWM);
    ComponentSafetyTestTimers::motorStartedAt = 0;
    logLine("[Test] Rotator auto-stopped after 30 s");
  }
}


// =====================================================
// FEED CYCLE: rotate -> settle -> servo drop
// =====================================================

void doFeed(int grams, int rotatorPwm) {
  if (feedInProgress) {
    logLine("[Feed] Ignored — a feed is already running");
    return;
  }
  feedInProgress = true;

  // A feed owns both actuators, so cancel any test deadline rather than
  // letting a timer fire part-way through the cycle.
  ComponentSafetyTestTimers::servoOpenedAt = 0;
  ComponentSafetyTestTimers::motorStartedAt = 0;

  feedOpenMs = ServoConfig::openMsForGrams(grams);

  logLine("[Feed] Dispensing " + String(grams) + " g  (open " +
          String(feedOpenMs) + " ms, rotator " +
          String((int)((constrain(rotatorPwm, 0, 255) / 255.0) * 100)) + "%)");

  // The LCD would otherwise sit frozen on whatever page it happened to
  // be showing. Say what is happening instead — this is the one moment
  // the machine moves. The servo phase updates the second line later.
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Feeding ");
  lcd.print(grams);
  lcd.print("g");
  lcd.setCursor(0, 1);
  lcd.print("Rotating...");

  // The long spin is deadline-driven, so loop() keeps serving WiFi and
  // cloud while the motor runs. serviceRotator() starts the servo phase
  // when the motor stops; finishFeed() releases the lock after that.
  startRotator(FeedController::ROTATOR_RUN_MS, rotatorPwm);
}


// =====================================================
// CLOUD: push one reading
// =====================================================

void pushReading() {
  if (WiFi.status() != WL_CONNECTED) return;

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

  String response;
  const int code = cloudRequest(String(SUPABASE_URL) + "/rest/v1/readings",
                                "POST", payload, &response, true);
  if (code == 200 || code == 201 || code == 204) {
    // Verbose: one of these every few seconds forever, so it only
    // reaches the app's console while somebody is watching it.
    logLine("[Cloud] Reading stored", true);
  } else {
    logLine("[Cloud] readings insert -> HTTP " + String(code));
    if (response.length()) logLine(response);   // PostgREST prints the reason
  }
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

  String url = String(SUPABASE_URL) +
               "/rest/v1/commands?select=id,type,grams,value,rotator_pwm,created_at"
               "&device_id=eq." + DEVICE_ID +
               "&handled=eq.false&order=created_at.asc&limit=1";

  String body;
  const int code = cloudRequest(url, "GET", "", &body);
  if (code != 200) return;

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

  // PATCH the one row: mark it handled so it isn't run twice.
  //
  // Only `handled` is set here. handled_at is left to the database (a
  // JSON string like "now()" is not a timestamptz literal, so sending
  // one would make PostgREST reject the whole PATCH with a 400).
  const int code = cloudRequest(
      String(SUPABASE_URL) + "/rest/v1/commands?id=eq." + id,
      "PATCH", "{\"handled\":true}", nullptr, true);

  const bool ok = (code == 200 || code == 204);
  if (!ok) {
    Serial.print("[Cloud] command ack -> HTTP ");
    Serial.println(code);
  }
  return ok;
}


// =====================================================
// OFFLINE: schedule cache on the board
// =====================================================

/// Mirror the in-RAM cache into Preferences so the next offline boot still
/// has the last set of feeds the app successfully synced.
void persistSchedules() {
  schedulePrefs.putInt("count", scheduleCount);
  for (int i = 0; i < scheduleCount; i++) {
    char slot[64] = {};
    encodeScheduleSlot(schedules[i].time, schedules[i].date,
                       schedules[i].grams, schedules[i].rotatorPwm,
                       schedules[i].active, slot, sizeof(slot));
    schedulePrefs.putString(("s" + String(i)).c_str(), slot);
  }
}

/// Restore the cache at boot. Until the first successful cloud poll this is
/// the schedule set checkScheduledFeeding() runs from.
void loadSchedules() {
  scheduleCount = schedulePrefs.getInt("count", 0);
  if (scheduleCount < 0) scheduleCount = 0;
  if (scheduleCount > MAX_SCHEDULES) scheduleCount = MAX_SCHEDULES;

  for (int i = 0; i < scheduleCount; i++) {
    String key = "s" + String(i);
    String slot = schedulePrefs.getString(key.c_str(), "");
    char time[6] = {};
    char date[11] = {};
    int grams = 10;
    int pwm = 255;
    bool active = true;
    if (!decodeScheduleSlot(slot.c_str(), time, sizeof(time), date, sizeof(date),
                            &grams, &pwm, &active)) {
      scheduleCount = i;   // stop at the first corrupt slot
      break;
    }
    strncpy(schedules[i].time, time, sizeof(schedules[i].time) - 1);
    schedules[i].time[sizeof(schedules[i].time) - 1] = '\0';
    strncpy(schedules[i].date, date, sizeof(schedules[i].date) - 1);
    schedules[i].date[sizeof(schedules[i].date) - 1] = '\0';
    schedules[i].grams = grams;
    schedules[i].rotatorPwm = pwm;
    schedules[i].active = active;
  }
  if (scheduleCount > 0) {
    Serial.print("[Offline] restored ");
    Serial.print(scheduleCount);
    Serial.println(" schedule(s) from the board");
  }
}


// =====================================================
// CLOUD: refresh feeding schedules
// =====================================================

// Returns true only when the cache was rebuilt, so the caller can retry a
// failed midnight refresh instead of treating the new date as handled.
bool refreshSchedules() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String url = String(SUPABASE_URL) +
               "/rest/v1/schedules?select=id,time,feed_date,grams,rotator_pwm,active"
               "&device_id=eq." + DEVICE_ID + "&active=eq.true";

  String body;
  const int code = cloudRequest(url, "GET", "", &body);
  if (code != 200) return false;

  // PostgREST returns a bare JSON array.
  StaticJsonDocument<3072> doc;
  if (deserializeJson(doc, body)) return false;

  JsonArray arr = doc.as<JsonArray>();
  scheduleCount = 0;
  for (JsonObject s : arr) {
    if (scheduleCount >= MAX_SCHEDULES) break;
    String t = s["time"].as<String>();
    strncpy(schedules[scheduleCount].time, t.c_str(), 5);
    schedules[scheduleCount].time[5] = '\0';
    // PostgREST returns SQL NULL as JSON null, not the string "null".
    if (s["feed_date"].isNull()) {
      schedules[scheduleCount].date[0] = '\0';
    } else {
      String d = s["feed_date"].as<String>();
      strncpy(schedules[scheduleCount].date, d.c_str(), 10);
      schedules[scheduleCount].date[10] = '\0';
    }
    schedules[scheduleCount].grams      = s["grams"] | 10;
    schedules[scheduleCount].rotatorPwm = s["rotator_pwm"] | 255;
    schedules[scheduleCount].active     = s["active"] | true;
    scheduleCount++;
  }
  logLine("[Cloud] Schedules loaded: " + String(scheduleCount));
  persistSchedules();
  return true;
}


// =====================================================
// SCHEDULED FEEDING  (fires each active schedule at its HH:MM)
// =====================================================

void checkScheduledFeeding() {
  struct tm t;
  if (!getLocalTime(&t, 50)) return;   // clock not ready yet
  timeSynced = true;

  char today[11] = "";
  formatScheduleDate(t, today, sizeof(today));

  // The 60 s poll may still hold yesterday's rows at midnight. Refresh as
  // soon as the UTC+8 date changes so a 00:00 feed is not missed. A failed
  // fetch leaves the date unhandled, but the retry is rate-limited to the
  // ordinary poll cadence: otherwise a cloud outage would block this 1 Hz
  // check for 10 s at a time and starve the AP and the sensors.
  if (strcmp(lastScheduleDate, today) != 0 &&
      millis() - lastScheduleDateAttempt >= SCHEDULE_DATE_RETRY_MS) {
    lastScheduleDateAttempt = millis();
    if (refreshSchedules()) {
      strncpy(lastScheduleDate, today, sizeof(lastScheduleDate) - 1);
      lastScheduleDate[sizeof(lastScheduleDate) - 1] = '\0';
    }
  }

  int minuteOfDay = t.tm_hour * 60 + t.tm_min;
  char feedKey[17] = "";
  buildFeedKey(today, minuteOfDay, feedKey, sizeof(feedKey));
  if (strcmp(feedKey, lastFedKey) == 0) return;   // already handled this minute

  char now[6];
  snprintf(now, sizeof(now), "%02d:%02d", t.tm_hour, t.tm_min);

  for (int i = 0; i < scheduleCount; i++) {
    if (!schedules[i].active) continue;
    if (!scheduleMatchesDate(schedules[i].date, today)) continue;
    if (strcmp(schedules[i].time, now) == 0) {
      logLine("[Schedule] " + String(now) + " -> feed " +
              String(schedules[i].grams) + " g");
      strncpy(lastFedKey, feedKey, sizeof(lastFedKey) - 1);
      lastFedKey[sizeof(lastFedKey) - 1] = '\0';   // claim BEFORE starting,
                                                   // so two rows at the same
                                                   // HH:MM cannot double-feed
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
  ComponentSafetyTestTimers::motorStartedAt = (motor1PWM > 0) ? millis() : 0;
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



/// Same bands, abbreviated for the 16-column LCD.
///
/// "0.53 mg/L Critical" is 18 characters and would be cut mid-word by
/// the panel; "0.53 mg/L Crit" is 14 and fits. The serial output and
/// the app still use the full words.




// =====================================================
// SERIAL SENSOR MONITOR
// =====================================================

// Built as whole lines so each one can go to Serial AND to the app's
// console through logLine(). These are the high-volume lines (every
// LocalSensorTiming::READ_INTERVAL_MS), so they are marked verbose: Serial always gets
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

  if (analogHeld) {
    LOG_V("            (rotator running - pH / turbidity / ammonia below are "
          "the last quiet readings, not fresh samples)");
  }

  // The bench's line: always the percentage and status, whatever the
  // ADC says.
  LOG_V("Turbidity:  " + String(relativeTurbidity, 2) + "% (" + turbidityStatus +
        ")   |   ADC: " + String(analogChannels[CH_TURBIDITY].raw) +
        "   ADC V: " + String(analogChannels[CH_TURBIDITY].pinVolts, 3) + " V");

  // Additive only — the percentage above is reported either way. This
  // just says the input is far below the calibrated 100% point
  // (TurbidityCalibration::V_HIGH = 1.1916 V), which has two very different causes:
  //
  //   - a deliberately covered or heavily fouled probe. Blocking the
  //     optical path drives the output below where TurbidityCalibration::V_HIGH was measured
  //     with a turbidity standard, so near-0 V is a REAL reading here,
  //     just off the end of the curve. 100% is the right answer.
  //
  //   - the board unpowered, the signal on the wrong pin, or a
  //     connector seated one position over. GPIO34 is input-only with
  //     no internal pull, so a steady zero means something is actively
  //     holding it down.
  //
  // Uncovering the probe tells them apart: a working sensor climbs back
  // toward TurbidityCalibration::V_CLEAR, a wiring fault stays pinned.
  if (analogChannels[CH_TURBIDITY].raw < TurbidityCalibration::ADC_MIN_VALID) {
    LOG_V("            ^ note: ADC is near 0 V, below the calibrated 100% "
          "point. Expected for a covered/fouled probe. If it stays there "
          "uncovered, check power, the A pin and the connector.");
  } else if (analogChannels[CH_TURBIDITY].raw > TurbidityCalibration::ADC_MAX_VALID) {
    LOG_V("            ^ note: ADC is railed HIGH - the signal wire is "
          "probably broken.");
  }

  // As for turbidity: the bench prints the converted pH unconditionally.
  LOG_V("pH:         " + String(pH, 2) +
        "   |   ADC: " + String(analogChannels[CH_PH].raw) +
        "   pin: " + String(analogChannels[CH_PH].pinVolts, 3) + " V" +
        "   probe: " + String(analogChannels[CH_PH].sensorVolts, 3) + " V");

  // Ammonia. Always a number now: mgLOf() clamps to the calibrated
  // span rather than reporting "-- out of range" or "Not Connected",
  // so the reading starts at 0 and the voltage is always shown beside
  // it. A clamped value is marked, because 0.00 at the floor and 0.53
  // at the ceiling are limits rather than measurements.
  LOG_V("NH3 ADC: " + String(analogChannels[CH_AMMONIA].raw) +
        "   ADC V: " + String(analogChannels[CH_AMMONIA].pinVolts, 3) + " V" +
        "   Sensor: " + String(analogChannels[CH_AMMONIA].sensorVolts, 3) + " V");

  {
    const float v = analogChannels[CH_AMMONIA].sensorVolts;
    String line = "Ammonia:    " + String(ammoniaEstimatedMgL, 2) + " mg/L" +
                  "   (" + String(v, 3) + " V)" +
                  "   Status: " + AmmoniaConfig::statusOf(ammoniaEstimatedMgL);

    if (AmmoniaConfig::belowRange(v)) {
      line += "   |   at floor: " + String(v, 3) + " V is below the " +
              String(AmmoniaConfig::V0, 3) + " V zero reference";
    } else if (AmmoniaConfig::aboveRange(v)) {
      line += "   |   OUT OF BAND (high): " + String(v, 3) + " V is " +
              String(v - AmmoniaConfig::V2, 3) + " V above the " +
              String(AmmoniaConfig::V2, 3) + " V top reference. Reading "
              "forced to 0.00 - the curve does not describe this sensor "
              "here, so this 0.00 is NOT proof the water is clean. "
              "Recalibrate the MQ-137 to get real mg/L.";
    }
    LOG_V(line);
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
      lcd.print(pH, 2);          // bench parity: always shown
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
      lcd.print(turbidityStatus);   // bench parity: always shown
      lcd.setCursor(0, 1);
      lcd.print(relativeTurbidity, 1);
      lcd.print("% turbid");
      break;

    case 3:  // Ammonia
      // Both lines sized for 16 columns: mg/L is always 4 characters
      // (0.00 .. 0.53) and the voltage 5, so the longest status word
      // still fits.
      lcd.setCursor(0, 0);
      lcd.print("Ammonia ");
      lcd.print(ammoniaEstimatedMgL, 2);
      lcd.print("mg/L");
      lcd.setCursor(0, 1);
      lcd.print(analogChannels[CH_AMMONIA].sensorVolts, 3);
      lcd.print("V ");
      lcd.print(AmmoniaConfig::statusOf(ammoniaEstimatedMgL));
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
  if (lcdPage >= LCDConfig::PAGE_COUNT) lcdPage = 0;
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

  // Restore the last synced schedule set before the first cloud poll, so a
  // board that boots with no WiFi can still feed on time.
  schedulePrefs.begin("crxsched", false);
  loadSchedules();

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
  myServo.write(ServoConfig::CLOSED_ANGLE);
  servoPosition = ServoConfig::CLOSED_ANGLE;

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

  // Raise / withdraw the offline access point and serve its API. Placed
  // right after checkWiFi() so it reacts in the same pass that the
  // station state changes.
  serviceAccessPoint();

  checkSerialCommand();

  // Stop the pre-feed rotation and hand over to the servo phase.
  serviceRotator();

  // Keep pushing the ADC block forward for as long as the rotator is
  // drawing current, so analog sampling resumes only once it has been
  // stopped for LocalSensorTiming::ADC_SETTLE_AFTER_MOTOR_MS. Done here rather than at
  // each motor1PWM write: this catches every path that energises it
  // (feed, schedule, Test Components, serial) without having to
  // remember to touch them all.
  if (motor1PWM > 0) {
    adcBlockedUntil = millis() + LocalSensorTiming::ADC_SETTLE_AFTER_MOTOR_MS;
  }

  // Close the feeder door / stop the rotator if a Test Components
  // command left either running. Must be before anything that blocks.
  checkTestTimeouts();

  // Local sensor refresh + fan control + serial print + LCD
  if (millis() - lastSensorRead >= LocalSensorTiming::READ_INTERVAL_MS) {
    lastSensorRead = millis();
    readSensors();
    controlCoolingMotor();
    printSensorData();
  }

  if (millis() - lastLCDUpdate >= LCDConfig::UPDATE_INTERVAL_MS) {
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
