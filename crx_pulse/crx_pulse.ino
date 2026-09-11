/**
 * CRX-Pulse — Complete Production Firmware (Single File) v6.0.0
 *
 * This revision implements honest, per-sensor connection tracking:
 * every sensor (pH, turbidity, ammonia, water level, water temp,
 * ambient temp) is validated INDEPENDENTLY before being included in
 * what's sent to the backend. A disconnected/invalid sensor is
 * OMITTED from the payload entirely - never sent as a fabricated
 * default, a stale value, or a clamped-into-range number pretending
 * to be real.
 *
 * Key changes from v5.0.0:
 *   - WaterLevelSensor: removed entirely - this build has no physical
 *     water level sensor wired, and won't have one, so the class,
 *     its struct fields, and every reference to it have been taken
 *     out rather than kept as a permanently-disabled stub.
 *   - pH/Turbidity/Ammonia: now validate their actual voltage range
 *     (a floating/disconnected analog pin reads near 0V or near the
 *     3.3V rail, not a plausible mid-range value) before being
 *     considered valid.
 *   - Ammonia (MQ-137) warm-up is now NON-BLOCKING - it used to
 *     freeze the entire boot sequence for 60 seconds before the
 *     device even became reachable. It now warms up in the
 *     background while everything else operates normally, and is
 *     reported as "not connected" until warm-up genuinely completes.
 *   - Real fan state (active + duty%) is now sent to the backend,
 *     not inferred by the app from ambient temperature.
 *   - LCD startup sequence rewritten to the exact 4-message flow
 *     requested: "Starting the system...", "Waiting for
 *     connection...", "Connecting...", "Connected".
 *   - The WiFi connection wait no longer fully blocks fan/local
 *     sensor updates - it now services them periodically during
 *     the connection attempt instead of a tight blocking loop.
 *
 * NOTE: Relay module has been physically removed from this build -
 * filter/lights/flush commands are accepted but log a
 * "no hardware" warning instead of controlling anything.
 *
 * v7.1.0 changes (WebSocket reverted, auto-connect/retry kept):
 *   - AUTO-CONNECT: if the device is already paired, it now joins
 *     WiFi automatically at boot instead of waiting for the app to
 *     send /connect. The /connect endpoint still exists (harmless -
 *     useful for a manual retry without power-cycling), but is no
 *     longer required for normal operation.
 *   - AUTO-RETRY: if a connection attempt fails and the device falls
 *     back to IDLE, it now automatically retries every 30 seconds on
 *     its own, indefinitely, with no app interaction needed.
 *   - Sensor data goes back to plain HTTP POST (SensorApi) - the
 *     WebSocket real-time layer was reverted. Polling-based, proven,
 *     simple - matches the current backend (no Channels/daphne
 *     required, plain `runserver` works again).
 *
 * Libraries needed (Arduino IDE Library Manager):
 *   ArduinoJson (6.x), ESP32Servo, LiquidCrystal_I2C, OneWire,
 *   DallasTemperature
 * (WebServer and Preferences ship built-in with the ESP32 core.)
 */

// ===================================================================
//  LIBRARY INCLUDES
// ===================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP32Servo.h>
// WebSocketsClient_Generic removed - no longer needed. Per the
// updated architecture, this device only ever makes outgoing HTTP
// requests (WebServer for the local AP/discovery servers is
// unrelated - that's the ESP32 acting as a server, not a client).
#include <vector>

// ===================================================================
//  CONFIG
// ===================================================================

#define WIFI_RETRY_DELAY_MS 500

// FastAPI + MongoDB backend - much simpler than Firebase's per-function
// URL scheme, since device_key resolves the account server-side on
// every request (no project ID/region path segments needed at all).
//
// UPDATE THIS IP to match your PC's CURRENT LAN IP (run `ipconfig`
// on Windows) - this WILL change if your router reassigns it, same
// requirement as every backend this project has used so far.

// ── STANDALONE MODE - no backend, no accounts ───────────────────
// The device joins your WiFi router directly and PERMANENTLY at a
// The ESP32 broadcasts its OWN WiFi network - your phone connects
// directly to it, no router, no home network, no band-matching
// concerns (2.4GHz vs 5GHz), no "are both devices on the same
// network" question ever again. This is the entire network
// configuration this firmware needs now.
//
// The ESP32's own AP always gets IP 192.168.4.1 automatically - this
// is fixed by the ESP32 WiFi library itself, not something you set.
#define ESP32_AP_SSID     "CRX-Pulse-ESP32"
#define ESP32_AP_PASSWORD "crxpulse123"   // minimum 8 characters, or "" for an open network

// The Flutter app polls this port directly on the AP's IP above -
// no backend, no login, fully local.
#define LOCAL_SERVER_PORT 8080

#define FIRMWARE_VERSION    "10.0.0"

#define PIN_PH              35
#define PIN_TURBIDITY       34
#define PIN_AMMONIA         39
#define PIN_TEMP_SENSOR      4

#define PIN_LCD_SDA         21
#define PIN_LCD_SCL         22
#define LCD_I2C_ADDR        0x27

#define PIN_LED_RED         17
#define PIN_LED_YELLOW       5
#define PIN_LED_GREEN       23
#define PIN_PIEZZO          16

#define PIN_SERVO           33

#define PIN_L298N_IN1       26
#define PIN_L298N_IN2       27
#define PIN_L298N_ENA       25

#define PIN_FAN_ENB         32

#define INTERVAL_SENSOR_MS    5000   // how often readings are refreshed and logged locally
#define INTERVAL_COMMAND_MS   5000
// AUTO_RECONNECT_RETRY_MS removed - per explicit request, the device
// no longer auto-retries connecting while idle. See the manual-only
// connect flow in setup()/loop() instead.
#define INTERVAL_FEEDING_MS  30000
#define SENSOR_SAMPLE_COUNT     20
#define AMMONIA_WARMUP_MS    60000
#define FAN_UPDATE_MS         2000
#define HEALTH_CHECK_MS       3000
#define LOCAL_LOOP_MS         3000

// Analog sensor validity bounds - a floating/disconnected pin reads
// near the rails (0V or 3.3V), not a plausible mid-range voltage.
#define SENSOR_MIN_VALID_VOLTAGE  0.05f
#define SENSOR_MAX_VALID_VOLTAGE  3.25f

#define PH_VOLTAGE_AT_7     1.50f
#define PH_VOLTS_PER_UNIT   0.18f
#define TURBIDITY_V_CLEAR   2.50f
#define AMMONIA_RO          10.0f
#define AMMONIA_RL          10.0f
#define AMMONIA_CLEAN_RATIO  3.6f

#define SERVO_HOME_POS       90
#define SERVO_OPEN_POS      180
#define SERVO_MIN_PULSE     500
#define SERVO_MAX_PULSE    2500
#define FEED_MIN_GRAMS      10.0f
#define FEED_MAX_GRAMS     500.0f
#define FEED_MIN_MS         200
#define FEED_MAX_MS        3000
#define MIN_FEED_INTERVAL_MS 60000

#define FAN_MIN_DUTY         51
#define FAN_MAX_DUTY        255
#define FAN_KICKSTART_DUTY  255
#define FAN_KICKSTART_MS    400
#define FAN_TEMP_LOW        28.0f
#define FAN_TEMP_HIGH       40.0f
#define FAN_PWM_FREQ       25000
#define FAN_PWM_RES             8
#define ENA_PWM_FREQ         5000
#define ENA_PWM_RES              8

#define PIEZO_PWM_FREQ       2000
#define PIEZO_PWM_RES           8
#define PIEZO_VOLUME_DUTY      60

#define NTP_SERVER_1       "pool.ntp.org"
#define NTP_SERVER_2       "time.nist.gov"
#define NTP_UTC_OFFSET     28800
#define NTP_DST_OFFSET     0

// ===================================================================
//  SENSORS
// ===================================================================

class ISensor
{
    public:
        virtual ~ISensor() = default;
        virtual bool begin() = 0;
        virtual float read() = 0;
        virtual const char* getName() const = 0;
        virtual const char* getUnit() const = 0;
        virtual bool isHealthy() const = 0;
        virtual bool isValid() const = 0;
        virtual float getLastReading() const = 0;
};

class SensorBase : public ISensor
{
    protected:
        uint8_t _pin;
        uint8_t _sampleCount;
        float _lastReading;
        float _lastVoltage;
        bool _initialized;
        bool _lastValid;

        float _averageVoltage()
        {
            long total = 0;
            for (uint8_t i = 0; i < _sampleCount; i++)
            {
                total += analogRead(_pin);
                delay(5);
            }
            _lastVoltage = (total / (float)_sampleCount) * (3.3f / 4095.0f);

            // A floating or disconnected analog pin reads pinned near
            // 0V or near the 3.3V rail - never a plausible mid-range
            // voltage a real sensor would produce. This is the actual
            // validity check requested: don't trust a reading just
            // because a number came back from analogRead().
            _lastValid = (_lastVoltage > SENSOR_MIN_VALID_VOLTAGE &&
                          _lastVoltage < SENSOR_MAX_VALID_VOLTAGE);

            return _lastVoltage;
        };

    public:
        explicit SensorBase(uint8_t pin, uint8_t sampleCount = SENSOR_SAMPLE_COUNT)
        : _pin(pin), _sampleCount(sampleCount), _lastReading(0.0f), _lastVoltage(0.0f),
          _initialized(false), _lastValid(false)
        {
        }

        bool begin() override
        {
            pinMode(_pin, INPUT);
            _initialized = true;
            return true;
        }

        bool isHealthy() const override
        {
            return _initialized;
        }

        bool isValid() const override
        {
            return _initialized && _lastValid;
        }

        float getLastReading() const override
        {
            return _lastReading;
        }
};

class PHSensor : public SensorBase
{
    private:
        float _voltageAtPH7;
        float _voltagePerUnit;
        float _waterTempC;

    public:
        explicit PHSensor(
            uint8_t pin = PIN_PH,
            float voltageAtPH7 = PH_VOLTAGE_AT_7,
            float voltagePerUnit = PH_VOLTS_PER_UNIT
        )
            : SensorBase(pin),
             _voltageAtPH7(voltageAtPH7),
             _voltagePerUnit(voltagePerUnit),
             _waterTempC(25.0f)  // standard reference temp probes are calibrated at
        {
        }

        /** Call before read() whenever a fresh water temperature is
         *  available - ACCURACY FIX: a glass-electrode pH probe's
         *  millivolt-per-pH slope follows the Nernst equation, which
         *  is directly proportional to ABSOLUTE temperature. A probe
         *  calibrated at 25 degC reads measurably wrong in water at,
         *  say, 30 degC without this correction - the error grows
         *  the further the reading is from pH 7 and the further the
         *  water temperature is from the 25 degC calibration point.
         *  Falls back to the 25 degC assumption (no correction) if
         *  this is never called or the temperature is invalid, which
         *  matches the ORIGINAL uncompensated behavior exactly - this
         *  can only make readings more accurate, never less. */
        void setWaterTempC(float tempC)
        {
            if (tempC > -20.0f && tempC < 60.0f)  // sanity bounds, not the sensor's own valid range
            {
                _waterTempC = tempC;
            }
        }

        float read() override
        {
            float voltage = _averageVoltage();

            // Nernst temperature compensation: slope scales linearly
            // with absolute temperature relative to the 25 degC
            // calibration reference.
            const float kelvinAtCalibration = 25.0f + 273.15f;
            float kelvinNow = _waterTempC + 273.15f;
            float compensatedSlope = _voltagePerUnit * (kelvinNow / kelvinAtCalibration);

            float ph = 7.0f + ((_voltageAtPH7 - voltage) / compensatedSlope);

            _lastReading = constrain(ph, 0.0f, 14.0f);
            return _lastReading;
        }

        void calibrate(float voltageAtPH7, float voltsPerUnit)
        {
            _voltageAtPH7 = voltageAtPH7;
            _voltagePerUnit = voltsPerUnit;
            Serial.println(F("[ph] Calibration updated."));
        }

        const char* getName() const override { return "pH Sensor V1.1"; }
        const char* getUnit() const override { return "pH"; }

        bool isSafeForCrayfish() const
        {
            return isValid() && _lastReading >= 7.0f && _lastReading <= 8.5f;
        }
};

class TurbiditySensor : public SensorBase
{
    private:
        float _voltageClear;

    public:
        explicit TurbiditySensor(
            uint8_t pin = PIN_TURBIDITY,
            float voltageClear = TURBIDITY_V_CLEAR
        )
            : SensorBase(pin)
            , _voltageClear(voltageClear)
        {
        }

        float read() override
        {
            float voltage = _averageVoltage();
            float ntu = _voltageToNTU(voltage);
            _lastReading = constrain(ntu, 0.0f, 3000.0f);
            return _lastReading;
        }

        void calibrateClear(float voltageInCleanWater)
        {
            _voltageClear = voltageInCleanWater;
            Serial.print(F("[Turbidity] Baseline set to"));
            Serial.print(_voltageClear, 3);
            Serial.println(F("V"));
        }

        const char* getName() const override { return "Turbidity Sensor V1.0"; }
        const char* getUnit() const override { return "NTU"; }

        bool isSafeForCrayfish() const
        {
            return isValid() && _lastReading < 50.0f;
        }

    private:
        float _voltageToNTU(float voltage) const
        {
            if (voltage >= _voltageClear)
            {
                return 0.0f;
            }

            if (voltage < 1.5f)
            {
                return 3000.0f;
            }

            float ratio = (_voltageClear - voltage) / (_voltageClear - 1.5f);
            return ratio * 3000.0f;
        }
};

class AmmoniaSensor : public SensorBase
{
    private:
        float _ro;
        float _rl;
        float _cleanAirRatio;
        bool  _calibrated;
        uint32_t _warmupStartMs;
        bool  _warmupStarted;
        bool  _warmedUp;
        float _ambientTempC;

    public:
        explicit AmmoniaSensor(
            uint8_t pin = PIN_AMMONIA,
            float ro = AMMONIA_RO,
            float rl = AMMONIA_RL,
            float cleanAirRatio = AMMONIA_CLEAN_RATIO
        )
            : SensorBase(pin)
            , _ro(ro)
            , _rl(rl)
            , _cleanAirRatio(cleanAirRatio)
            , _calibrated(false)
            , _warmupStartMs(0)
            , _warmupStarted(false)
            , _warmedUp(false)
            , _ambientTempC(20.0f)  // MQ-series datasheets reference 20 degC
        {
        }

        /** Call before read() whenever a fresh ambient/housing
         *  temperature is available - ACCURACY FIX: MQ-series gas
         *  sensors (MQ-137 included) have an Rs/Ro ratio that shifts
         *  with temperature per the manufacturer's published
         *  correction curve, since the sensing element's conductivity
         *  is itself temperature-dependent. This applies a
         *  conservative linear approximation of that correction
         *  (roughly -2%/degC relative to the 20 degC reference, a
         *  commonly used approximation when the full datasheet curve
         *  isn't available in closed form) - meaningfully better than
         *  no compensation at all, though a full calibration against
         *  the exact datasheet curve would be more precise still.
         *  Falls back to the 20 degC assumption if never called. */
        void setAmbientTempC(float tempC)
        {
            if (tempC > -20.0f && tempC < 60.0f)
            {
                _ambientTempC = tempC;
            }
        }

        float read() override
        {
            float voltage = _averageVoltage();

            if (!_lastValid || voltage <= 0.01f)
            {
                _lastReading = 0.0f;
                return 0.0f;
            }

            float rs = _voltageToResistance(voltage);

            // Conservative linear temperature correction (~-2%/degC
            // from the 20 degC reference) applied to Rs before the
            // ratio is computed - see setAmbientTempC() above.
            float tempDelta = _ambientTempC - 20.0f;
            float correctionFactor = 1.0f - (0.02f * tempDelta);
            correctionFactor = constrain(correctionFactor, 0.5f, 1.5f);  // never let a wild reading apply an absurd correction
            rs *= correctionFactor;

            float ppm = _resistanceToPPM(rs);

            _lastReading = constrain(ppm, 0.0f, 1000.0f);
            return _lastReading;
        }

        /** Call once, right after begin() - starts the non-blocking
         *  warm-up timer without freezing the rest of the system. */
        void beginWarmup()
        {
            _warmupStartMs = millis();
            _warmupStarted = true;
            Serial.println(F("[NH3] Warm-up started (non-blocking, running in background)"));
        }

        /** Call every loop() iteration - checks elapsed time and
         *  performs the one-time calibration once warm-up completes.
         *  Does NOT block anything while waiting. */
        void updateWarmup(uint32_t warmupMs = AMMONIA_WARMUP_MS)
        {
            if (!_warmupStarted || _warmedUp) return;

            if (millis() - _warmupStartMs >= warmupMs)
            {
                calibrateInCleanAir();
                _warmedUp = true;
                Serial.println(F("[NH3] Warm-up complete - sensor now reporting."));
            }
        }

        void calibrateInCleanAir()
        {
            Serial.println(F("[NH3] Calibrating in clean air..."));

            float voltage = _averageVoltage();
            float rs = _voltageToResistance(voltage);
            _ro = rs / _cleanAirRatio;
            _calibrated = true;

            Serial.print(F("[NH3] Ro = "));
            Serial.print(_ro, 2);
            Serial.println(F(" k(Ohms)"));
        }

        bool isCalibrated() const { return _calibrated; }
        bool isWarmedUp() const { return _warmedUp; }

        // Ammonia is only genuinely "valid" once BOTH the voltage is
        // in a plausible range AND the warm-up/calibration has
        // actually completed - reporting a reading before warm-up
        // finishes would be exactly the kind of misleading data this
        // update is meant to eliminate.
        bool isValid() const override
        {
            return SensorBase::isValid() && _warmedUp && _calibrated;
        }

        const char* getName() const override { return "MQ-137 Ammonia"; }
        const char* getUnit() const override { return "ppm"; }

        bool isSafeForCrayfish() const
        {
            return isValid() && _lastReading < 0.5f;
        }

    private:
        float _voltageToResistance(float voltage) const
        {
            return _rl * (3.3f - voltage) / voltage;
        }

        float _resistanceToPPM(float rs) const
        {
            float ratio = rs / _ro;
            return 116.6020682f * powf(ratio, -2.769034857f);
        }
};

class DS18B20Sensor {
private:
    OneWire _oneWire;
    DallasTemperature _sensors;
    int _deviceCount;
    DeviceAddress _waterAddr;
    DeviceAddress _ambientAddr;
    bool _waterAddrValid;
    bool _ambientAddrValid;

public:
    DS18B20Sensor()
        : _oneWire(PIN_TEMP_SENSOR)
        , _sensors(&_oneWire)
        , _deviceCount(0)
        , _waterAddrValid(false)
        , _ambientAddrValid(false)
    {}

    bool begin() {
        _sensors.begin();
        _deviceCount = _sensors.getDeviceCount();

        Serial.print(F("[DS18B20] Devices found: "));
        Serial.println(_deviceCount);

        if (_deviceCount < 2) {
            Serial.println(F("[DS18B20] Expected 2 probes (water + ambient), found fewer - ambient/housing temp will report as Not Connected"));
        }

        // ACCURACY FIX: capture each probe's unique 64-bit ROM address
        // ONCE at boot, and use THAT for every subsequent read instead
        // of getTempCByIndex(). Reading by bus index is fragile - the
        // DallasTemperature library enumerates devices in whatever
        // order it discovers them on the OneWire bus, which is not
        // contractually guaranteed to stay in the same order across
        // power cycles or after any bus disturbance. Reading by the
        // address captured here guarantees "water" and "ambient"
        // always refer to the same physical probe for the rest of
        // this power-up session, regardless of enumeration order.
        if (_deviceCount >= 1) {
            _waterAddrValid = _sensors.getAddress(_waterAddr, 0);
            if (!_waterAddrValid) {
                Serial.println(F("[DS18B20] getAddress(0) failed despite device being counted - falling back to index-based read for water temp"));
            }
        }
        if (_deviceCount >= 2) {
            _ambientAddrValid = _sensors.getAddress(_ambientAddr, 1);
            if (!_ambientAddrValid) {
                Serial.println(F("[DS18B20] getAddress(1) failed despite device being counted - falling back to index-based read for ambient temp"));
            }
        }

        // ACCURACY FIX: explicit 12-bit resolution (0.0625 degC steps,
        // the DS18B20's maximum) - the library's default depends on
        // the specific clone/batch and isn't guaranteed to already be
        // 12-bit. Conversion takes longer at 12-bit (~750ms) but
        // requestTemperatures() already blocks until ready by default,
        // so callers don't need to change anything to benefit.
        if (_waterAddrValid) _sensors.setResolution(_waterAddr, 12);
        if (_ambientAddrValid) _sensors.setResolution(_ambientAddr, 12);

        return _deviceCount >= 1;
    }

    int getDeviceCount() const { return _deviceCount; }

    void requestAll() {
        _sensors.requestTemperatures();
    }

    float readWaterTempC() {
        // FIX: fall back to index-based read if address capture
        // failed at boot, rather than permanently reporting "Not
        // Connected" for a probe that IS actually present and was
        // counted - a failed getAddress() call should degrade
        // gracefully, not lose the reading outright.
        if (_waterAddrValid) return _sensors.getTempC(_waterAddr);
        if (_deviceCount >= 1) return _sensors.getTempCByIndex(0);
        return DEVICE_DISCONNECTED_C;
    }

    float readAmbientTempC() {
        if (_ambientAddrValid) return _sensors.getTempC(_ambientAddr);
        if (_deviceCount >= 2) return _sensors.getTempCByIndex(1);
        return DEVICE_DISCONNECTED_C;
    }

    bool isValid(float tempC) const {
        return tempC != DEVICE_DISCONNECTED_C && tempC > -55.0f && tempC < 125.0f;
    }

    const char* getName() const { return "DS18B20 Dual Probe"; }
};

/** Aggregated readings - every *Valid flag reflects the ACTUAL,
 *  independently-checked state of that specific component. */
struct SensorReadings {
    float ph;
    float ammonia;
    float turbidity;
    float water_temp_c;
    float ambient_temp_c;

    bool  phValid;
    bool  ammoniaValid;
    bool  turbidityValid;
    bool  waterTempValid;
    bool  ambientTempValid;

    bool  fanActive;
    int   fanDutyPercent;
};

class SensorManager {
private:
    PHSensor          _phSensor;
    TurbiditySensor   _turbiditySensor;
    AmmoniaSensor     _ammoniaSensor;
    DS18B20Sensor     _tempSensor;

    std::vector<ISensor*> _sensors;

public:
    SensorManager()
        : _phSensor()
        , _turbiditySensor()
        , _ammoniaSensor()
        , _tempSensor()
    {
        _sensors = {
            &_phSensor,
            &_turbiditySensor,
            &_ammoniaSensor,
        };
    }

    bool begin() {
        bool allOk = true;
        for (ISensor* sensor : _sensors) {
            bool ok = sensor->begin();
            Serial.print(F("[SensorManager] "));
            Serial.print(sensor->getName());
            Serial.println(ok ? F(" Ready") : F(" Failed"));
            if (!ok) allOk = false;
        }

        bool tempOk = _tempSensor.begin();
        Serial.print(F("[SensorManager] "));
        Serial.print(_tempSensor.getName());
        Serial.println(tempOk ? F(" Ready") : F(" Failed"));
        if (!tempOk) allOk = false;

        return allOk;
    }

    /** Non-blocking - just starts the ammonia warm-up timer. */
    void beginAmmoniaWarmup() {
        _ammoniaSensor.beginWarmup();
    }

    /** Call every loop() iteration - completes ammonia warm-up in
     *  the background without freezing anything else. */
    void updateAmmoniaWarmup() {
        _ammoniaSensor.updateWarmup(AMMONIA_WARMUP_MS);
    }

    bool isAmmoniaReady() const {
        return _ammoniaSensor.isWarmedUp();
    }

    SensorReadings readAll(bool fanActive = false, int fanDutyPercent = 0) {
        SensorReadings readings;

        // ACCURACY FIX: temperatures are read FIRST now, and fed into
        // the pH and ammonia sensors' temperature compensation BEFORE
        // their own read() is called - both corrections need this
        // session's actual temperature, not whatever was left over
        // from the previous loop() iteration (or the 25/20 degC
        // defaults, on the very first read before any temp is known).
        _tempSensor.requestAll();
        readings.water_temp_c   = _tempSensor.readWaterTempC();
        readings.ambient_temp_c = _tempSensor.readAmbientTempC();
        readings.waterTempValid   = _tempSensor.isValid(readings.water_temp_c);
        readings.ambientTempValid = _tempSensor.isValid(readings.ambient_temp_c);

        if (readings.waterTempValid)   _phSensor.setWaterTempC(readings.water_temp_c);
        if (readings.ambientTempValid) _ammoniaSensor.setAmbientTempC(readings.ambient_temp_c);

        readings.ph = _phSensor.read();
        readings.phValid = _phSensor.isValid();

        readings.turbidity = _turbiditySensor.read();
        readings.turbidityValid = _turbiditySensor.isValid();

        readings.ammonia = _ammoniaSensor.read();
        readings.ammoniaValid = _ammoniaSensor.isValid();


        readings.fanActive = fanActive;
        readings.fanDutyPercent = fanDutyPercent;

        return readings;
    }

    void printReadings(const SensorReadings& r) const {
        Serial.println(F("---------------------------------------"));
        Serial.println(F("         SENSOR READINGS               "));
        Serial.println(F("---------------------------------------"));
        _printRow("pH",          r.phValid,        r.ph,          "pH");
        _printRow("Ammonia",     r.ammoniaValid,   r.ammonia,     "ppm");
        _printRow("Turbidity",   r.turbidityValid, r.turbidity,   "NTU");
        _printTempRow("Water Temp",   r.waterTempValid,   r.water_temp_c);
        _printTempRow("Ambient Temp", r.ambientTempValid, r.ambient_temp_c);
        Serial.print(F("  Fan: "));
        Serial.print(r.fanActive ? F("ON") : F("OFF"));
        Serial.print(F(" @ "));
        Serial.print(r.fanDutyPercent);
        Serial.println(F("%"));
        Serial.println(F("---------------------------------------"));
    }

    PHSensor&         getPHSensor()         { return _phSensor;         }
    TurbiditySensor&  getTurbiditySensor()  { return _turbiditySensor;  }
    AmmoniaSensor&    getAmmoniaSensor()    { return _ammoniaSensor;    }
    DS18B20Sensor&    getTempSensor()       { return _tempSensor;       }

private:
    void _printRow(const char* label, bool valid, float value, const char* unit) const {
        Serial.print(F("  "));
        Serial.print(label);
        Serial.print(F(": "));
        if (valid) {
            Serial.print(value, 2);
            Serial.print(F(" "));
            Serial.println(unit);
        } else {
            Serial.println(F("NOT CONNECTED"));
        }
    }

    void _printTempRow(const char* label, bool valid, float value) const {
        Serial.print(F("  "));
        Serial.print(label);
        Serial.print(F(": "));
        if (valid) {
            Serial.print(value, 1);
            Serial.println(F(" C"));
        } else {
            Serial.println(F("NOT CONNECTED"));
        }
    }
};

// ===================================================================
//  ACTUATORS
// ===================================================================

class IActuator
{
    public:
        virtual ~IActuator() = default;

        virtual bool begin() = 0;
        virtual void activate() = 0;
        virtual void deactivate() = 0;
        virtual bool isActive() const = 0;
        virtual const char* getName() const = 0;
};

class ServoActuator : public IActuator {
private:
    Servo       _servo;
    uint8_t     _pin;
    const char* _zoneName;
    bool        _active;
    bool        _attached;

public:
    explicit ServoActuator(uint8_t pin, const char* zoneName)
        : _pin(pin)
        , _zoneName(zoneName)
        , _active(false)
        , _attached(false)
    {}

    bool begin() override {
        _attached = _servo.attach(_pin, SERVO_MIN_PULSE, SERVO_MAX_PULSE);
        _servo.write(SERVO_HOME_POS);
        delay(300);
        Serial.print(F("[Servo] "));
        Serial.print(_zoneName);
        Serial.println(_attached ? F(" -> HOME (attached)") : F(" -> FAILED TO ATTACH"));
        return _attached;
    }

    void activate() override {
        _servo.write(SERVO_OPEN_POS);
        _active = true;
    }

    void deactivate() override {
        _servo.write(SERVO_HOME_POS);
        _active = false;
    }

    bool isActive() const override { return _active; }
    bool isAttached() const { return _attached; }

    const char* getName() const override { return _zoneName; }

    void dispense(float amountGrams) {
        if (!_attached) {
            Serial.print(F("[Servo] "));
            Serial.print(_zoneName);
            Serial.println(F(" - cannot dispense, servo not attached"));
            return;
        }

        uint32_t openTimeMs = _gramsToMs(amountGrams);

        Serial.print(F("[Servo]  "));
        Serial.print(_zoneName);
        Serial.print(F(" -> dispensing "));
        Serial.print(amountGrams, 1);
        Serial.print(F("g ("));
        Serial.print(openTimeMs);
        Serial.println(F("ms)"));

        activate();
        delay(openTimeMs);
        deactivate();

        Serial.print(F("[Servo]  "));
        Serial.print(_zoneName);
        Serial.println(F(" done"));
    }

    void moveTo(int degrees) {
        _servo.write(constrain(degrees, 0, 180));
    }

private:
    uint32_t _gramsToMs(float grams) const {
        float clamped = constrain(grams, FEED_MIN_GRAMS, FEED_MAX_GRAMS);
        return (uint32_t)map(
            (long)clamped,
            (long)FEED_MIN_GRAMS,
            (long)FEED_MAX_GRAMS,
            FEED_MIN_MS,
            FEED_MAX_MS
        );
    }
};

class FeedingMechanism {
private:
    ServoActuator _zone1;
    unsigned long _lastFeedMs = 0;

public:
    FeedingMechanism()
        : _zone1(PIN_SERVO, "Zone 1")
    {}

    bool begin() {
        bool ok = _zone1.begin();
        Serial.println(F("[FeedingMechanism] Feeder ready"));
        return ok;
    }

    bool isServoAttached() const {
        return _zone1.isAttached();
    }

    bool dispenseToZone(const String& zone, float amountGrams) {
        if (zone != "zone_1") {
            Serial.print(F("[FeedingMechanism] Unknown zone: "));
            Serial.println(zone);
            return false;
        }
        return feedNow(amountGrams);
    }

    bool feedNow(float amountGrams) {
        unsigned long now = millis();
        if (_lastFeedMs != 0 && (now - _lastFeedMs) < MIN_FEED_INTERVAL_MS) {
            unsigned long remainingS = (MIN_FEED_INTERVAL_MS - (now - _lastFeedMs)) / 1000;
            Serial.print(F("[FeedingMechanism] Feed blocked - cooldown active ("));
            Serial.print(remainingS);
            Serial.println(F("s remaining)"));
            return false;
        }

        Serial.println(F("[FeedingMechanism] Feed triggered"));
        _zone1.dispense(amountGrams);
        _lastFeedMs = now;
        return true;
    }

    ServoActuator& getZone1() { return _zone1; }
};

/** FanController now tracks and exposes its REAL state at all times -
 *  isActive() and getDutyPercent() reflect exactly what's being
 *  driven on the hardware, not an assumption. */
class FanController : public IActuator {
private:
    int _currentDuty = 0;
    uint32_t _lastUpdate = 0;
    bool _active = false;
    bool _initialized = false;

public:
    bool begin() override {
        ledcAttach(PIN_FAN_ENB, FAN_PWM_FREQ, FAN_PWM_RES);

        Serial.println(F("[FanController] Kick-starting..."));
        ledcWrite(PIN_FAN_ENB, FAN_KICKSTART_DUTY);
        delay(FAN_KICKSTART_MS);
        ledcWrite(PIN_FAN_ENB, FAN_MIN_DUTY);
        _currentDuty = FAN_MIN_DUTY;
        _active = true;
        _initialized = true;
        Serial.println(F("[FanController] Settled to 20% baseline speed"));
        return true;
    }

    void activate() override { }
    void deactivate() override { ledcWrite(PIN_FAN_ENB, 0); _currentDuty = 0; _active = false; }

    // Real state - true only if begin() succeeded AND duty is above 0.
    bool isActive() const override { return _initialized && _active && _currentDuty > 0; }

    const char* getName() const override { return "Cooling Fan"; }

    void update(float ambientTempC, bool ambientValid) {
        uint32_t now = millis();
        if (now - _lastUpdate < FAN_UPDATE_MS) return;
        _lastUpdate = now;

        // Without a valid ambient reading, fall back to the safe
        // baseline speed rather than guessing - this is itself an
        // honest, documented behavior (never silently assume "hot").
        if (!ambientValid) {
            if (_currentDuty != FAN_MIN_DUTY) {
                _currentDuty = FAN_MIN_DUTY;
                ledcWrite(PIN_FAN_ENB, _currentDuty);
            }
            return;
        }

        int duty;
        if (ambientTempC <= FAN_TEMP_LOW) {
            duty = FAN_MIN_DUTY;
        } else if (ambientTempC >= FAN_TEMP_HIGH) {
            duty = FAN_MAX_DUTY;
        } else {
            float ratio = (ambientTempC - FAN_TEMP_LOW) / (FAN_TEMP_HIGH - FAN_TEMP_LOW);
            duty = FAN_MIN_DUTY + static_cast<int>(ratio * (FAN_MAX_DUTY - FAN_MIN_DUTY));
        }

        if (_currentDuty == 0 && duty > 0) {
            ledcWrite(PIN_FAN_ENB, FAN_KICKSTART_DUTY);
            delay(FAN_KICKSTART_MS);
        }

        if (duty != _currentDuty) {
            _currentDuty = duty;
            ledcWrite(PIN_FAN_ENB, _currentDuty);
        }
    }

    int getDutyPercent() const { return (_currentDuty * 100) / 255; }
};

// ===================================================================
//  LOCAL API SERVER — fully offline, no backend, no accounts
// ===================================================================
//
// Per the pivot away from the FastAPI backend: this ESP32 now runs
// entirely standalone. It connects to WiFi using the hardcoded
// credentials below and serves live sensor data + a feed trigger
// directly to whichever app on the local network asks for it - no
// login, no device_key, no pairing flow, no MongoDB history. The
// Flutter app polls this directly.
class LocalApiServer {
private:
    WebServer _server;
    SensorManager& _sensorManager;
    FeedingMechanism& _feedingMechanism;
    FanController& _fanController;
    SensorReadings* _latestReadings;

public:
    LocalApiServer(SensorManager& sensorManager, FeedingMechanism& feedingMechanism, FanController& fanController, SensorReadings* latestReadings)
        : _server(LOCAL_SERVER_PORT)
        , _sensorManager(sensorManager)
        , _feedingMechanism(feedingMechanism)
        , _fanController(fanController)
        , _latestReadings(latestReadings)
    {}

    void begin() {
        _server.on("/status", HTTP_GET, [this]() { _handleStatus(); });
        _server.on("/feed", HTTP_POST, [this]() { _handleFeed(); });
        _server.begin();
        Serial.print(F("[LocalApi] Server started on port "));
        Serial.println(LOCAL_SERVER_PORT);
    }

    void handleClient() {
        _server.handleClient();
    }

private:
    void _addCors() {
        _server.sendHeader("Access-Control-Allow-Origin", "*");
    }

    /** Same honesty principle as the old backend: a disconnected/
     *  invalid sensor is OMITTED from the JSON entirely, never sent
     *  as a fake zero - the app shows "Not Connected" for anything
     *  missing rather than a misleading number. */
    void _handleStatus() {
        _addCors();
        DynamicJsonDocument doc(512);
        const SensorReadings& r = *_latestReadings;

        if (r.phValid) doc["ph"] = serialized(String(r.ph, 2));
        if (r.ammoniaValid) doc["ammonia"] = serialized(String(r.ammonia, 3));
        if (r.turbidityValid) doc["turbidity"] = serialized(String(r.turbidity, 1));
        if (r.waterTempValid) doc["water_temp_c"] = serialized(String(r.water_temp_c, 1));
        if (r.ambientTempValid) doc["ambient_temp_c"] = serialized(String(r.ambient_temp_c, 1));

        doc["fan_active"] = _fanController.isActive();
        doc["fan_duty_percent"] = _fanController.getDutyPercent();
        doc["mac_address"] = WiFi.macAddress();
        doc["firmware_version"] = FIRMWARE_VERSION;

        String out;
        serializeJson(doc, out);
        _server.send(200, "application/json", out);
    }

    void _handleFeed() {
        _addCors();
        Serial.println(F("[LocalApi] Feed triggered via app"));
        _feedingMechanism.feedNow(100.0f);
        _server.send(200, "application/json", "{\"success\":true}");
    }
};


// ===================================================================
//  DISPLAY & STATUS
// ===================================================================

class LcdDashboard {
private:
    LiquidCrystal_I2C _lcd;

public:
    LcdDashboard() : _lcd(LCD_I2C_ADDR, 16, 2) {}

    bool begin() {
        Wire.begin(PIN_LCD_SDA, PIN_LCD_SCL);
        // FIX: PCF8574-based I2C LCD backpacks (this one uses the
        // common 0x27 address) need a short settle time after the
        // I2C bus comes up and before init() - calling init()
        // immediately is a well-known cause of garbled/corrupted
        // characters on the FIRST screen shown after power-on,
        // since the LCD controller itself hasn't finished its own
        // internal power-on reset yet.
        delay(50);
        _lcd.init();
        _lcd.backlight();
        _lcd.clear();  // some clones retain garbage in DDRAM through init() alone
        return isResponding();
    }

    bool isResponding() {
        Wire.beginTransmission(LCD_I2C_ADDR);
        return Wire.endTransmission() == 0;
    }

    void showDashboard(int workingCount, int totalCount) {
        _lcd.clear();
        _lcd.setCursor(0, 0);
        _lcd.print("CRX PULSE");
        _lcd.setCursor(0, 1);
        _lcd.print("COMPONENTS ");
        _lcd.print(workingCount);
        _lcd.print("/");
        _lcd.print(totalCount);
    }

    void showRaw(const char* line1, const char* line2) {
        _lcd.clear();
        _lcd.setCursor(0, 0);
        _lcd.print(line1);
        _lcd.setCursor(0, 1);
        _lcd.print(line2);
    }
};

enum class WifiLedState : uint8_t { RED, YELLOW, GREEN };

class StatusIndicator {
private:
    WifiLedState _current = WifiLedState::YELLOW;

public:
    void begin() {
        pinMode(PIN_LED_RED, OUTPUT);
        pinMode(PIN_LED_YELLOW, OUTPUT);
        pinMode(PIN_LED_GREEN, OUTPUT);

        ledcAttach(PIN_PIEZZO, PIEZO_PWM_FREQ, PIEZO_PWM_RES);
        ledcWrite(PIN_PIEZZO, 0);
    }

    void setState(WifiLedState state) {
        if (state == _current) return;
        digitalWrite(PIN_LED_RED,    state == WifiLedState::RED    ? HIGH : LOW);
        digitalWrite(PIN_LED_YELLOW, state == WifiLedState::YELLOW ? HIGH : LOW);
        digitalWrite(PIN_LED_GREEN,  state == WifiLedState::GREEN  ? HIGH : LOW);
        _current = state;
    }

    /** Toggles the yellow LED on/off every ~400ms - call this every
     *  loop() iteration while waiting for a phone to connect, instead
     *  of setState() (which only supports a solid, unchanging color).
     *  Turns off red/green so only yellow is ever lit while blinking. */
    void blinkYellow(unsigned long now) {
        static unsigned long lastToggle = 0;
        static bool on = false;
        if (now - lastToggle >= 400) {
            lastToggle = now;
            on = !on;
            digitalWrite(PIN_LED_YELLOW, on ? HIGH : LOW);
            digitalWrite(PIN_LED_RED, LOW);
            digitalWrite(PIN_LED_GREEN, LOW);
            _current = WifiLedState::YELLOW;  // keeps setState() honest about current color
        }
    }

    void softBeep() {
        ledcWrite(PIN_PIEZZO, PIEZO_VOLUME_DUTY);
        delay(60);
        ledcWrite(PIN_PIEZZO, 0);
        delay(80);
        ledcWrite(PIN_PIEZZO, PIEZO_VOLUME_DUTY);
        delay(60);
        ledcWrite(PIN_PIEZZO, 0);
    }

    void beepConnecting() {
        ledcWrite(PIN_PIEZZO, PIEZO_VOLUME_DUTY / 2);
        delay(40);
        ledcWrite(PIN_PIEZZO, 0);
    }

    void beepSuccess() {
        ledcWriteTone(PIN_PIEZZO, 1400);
        delay(80);
        ledcWriteTone(PIN_PIEZZO, 2000);
        delay(100);
        ledcWrite(PIN_PIEZZO, 0);
    }

    void beepFailure() {
        ledcWriteTone(PIN_PIEZZO, 1000);
        delay(120);
        ledcWriteTone(PIN_PIEZZO, 600);
        delay(150);
        ledcWrite(PIN_PIEZZO, 0);
    }

    void beepServerConnecting() {
        ledcWriteTone(PIN_PIEZZO, 900);
        delay(50);
        ledcWriteTone(PIN_PIEZZO, 1100);
        delay(50);
        ledcWriteTone(PIN_PIEZZO, 1300);
        delay(50);
        ledcWrite(PIN_PIEZZO, 0);
    }

    void beepServerSuccess() {
        ledcWriteTone(PIN_PIEZZO, 1200);
        delay(90);
        ledcWriteTone(PIN_PIEZZO, 1600);
        delay(90);
        ledcWriteTone(PIN_PIEZZO, 2200);
        delay(140);
        ledcWrite(PIN_PIEZZO, 0);
    }

    void beepServerFailure() {
        ledcWriteTone(PIN_PIEZZO, 700);
        delay(100);
        ledcWriteTone(PIN_PIEZZO, 500);
        delay(100);
        ledcWriteTone(PIN_PIEZZO, 300);
        delay(180);
        ledcWrite(PIN_PIEZZO, 0);
    }

    void beepBootJingle() {
        const int notes[] = { 523, 659, 784, 1047 };
        const int noteDurationMs = 90;
        const int gapMs = 20;

        for (int i = 0; i < 4; i++) {
            ledcWriteTone(PIN_PIEZZO, notes[i]);
            delay(noteDurationMs);
            ledcWrite(PIN_PIEZZO, 0);
            delay(gapMs);
        }
        ledcWriteTone(PIN_PIEZZO, 1047);
        delay(60);
        ledcWrite(PIN_PIEZZO, 0);
        delay(40);
        ledcWriteTone(PIN_PIEZZO, 1319);
        delay(120);
        ledcWrite(PIN_PIEZZO, 0);
    }

    void beepTriggerReceived() {
        ledcWriteTone(PIN_PIEZZO, 1500);
        delay(50);
        ledcWrite(PIN_PIEZZO, 0);
        delay(60);
        ledcWriteTone(PIN_PIEZZO, 1500);
        delay(50);
        ledcWrite(PIN_PIEZZO, 0);
    }
};

// ===================================================================
//  GLOBAL OBJECTS
// ===================================================================

// SystemState/CONNECTING/etc. simplified - the device is either
// still starting up, or fully running (connected to WiFi and serving
// local requests). There's no backend to fail to reach anymore, no
// "provisioned" concept - just WiFi connected or not.
enum class SystemState : uint8_t { STARTING, RUNNING };
SystemState systemState = SystemState::STARTING;

LcdDashboard     lcdDashboard;
StatusIndicator  statusIndicator;

SensorManager    sensorManager;
FeedingMechanism feedingMechanism;
FanController    fanController;

SensorReadings   latestReadings;
LocalApiServer*  localApiServer = nullptr;
bool             phoneWasConnected = false;   // tracks the LAST known station count, so we only act on genuine transitions

constexpr int TOTAL_COMPONENTS = 8;
bool componentOK[TOTAL_COMPONENTS];

unsigned long lastLocalLoop = 0;
unsigned long lastSensorTime = 0, lastFeedingTime = 0, lastHealthCheck = 0;

void runHealthCheck(const SensorReadings& r);
int  countWorking();

// ===================================================================
//  SETUP
// ===================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println(F("=== CRX-Pulse Boot - fully offline, no backend ==="));

    lcdDashboard.begin();
    statusIndicator.begin();

    lcdDashboard.showRaw("Starting the", "system...");
    statusIndicator.beepBootJingle();

    sensorManager.begin();
    feedingMechanism.begin();
    fanController.begin();
    sensorManager.beginAmmoniaWarmup();

    lcdDashboard.showRaw("Starting", "WiFi...");
    statusIndicator.setState(WifiLedState::YELLOW);

    // Broadcasting our own network - no router, no connection
    // attempt that can fail or time out. This always succeeds
    // immediately; there's no "connect" step to wait on at all.
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ESP32_AP_SSID, ESP32_AP_PASSWORD);
    Serial.print(F("[Boot] Broadcasting: "));
    Serial.println(ESP32_AP_SSID);
    Serial.print(F("[Boot] Connect your phone to this network, then reach the device at: "));
    Serial.println(WiFi.softAPIP());

    // Stage 1: "WiFi is activated" - the AP itself is up, solid
    // yellow, shown briefly before moving into the waiting stage.
    lcdDashboard.showRaw("WiFi is", "activated");
    statusIndicator.setState(WifiLedState::YELLOW);
    statusIndicator.beepSuccess();
    delay(1200);

    // Stage 2: "Connecting to WiFi" - waiting for a phone to actually
    // join the AP, blinking yellow. loop() takes over checking for
    // the transition to Connected from here.
    lcdDashboard.showRaw("Connecting to", "WiFi");
    Serial.println(F("[Boot] Waiting for a phone to connect..."));

    localApiServer = new LocalApiServer(sensorManager, feedingMechanism, fanController, &latestReadings);
    localApiServer->begin();

    lastSensorTime = lastFeedingTime = lastHealthCheck = millis();
    latestReadings = sensorManager.readAll(fanController.isActive(), fanController.getDutyPercent());
    runHealthCheck(latestReadings);

    systemState = SystemState::RUNNING;
    Serial.println(F("CRX-PULSE running standalone - connect your phone directly to this device's WiFi"));
}

// ===================================================================
//  LOOP
// ===================================================================

void loop() {
    sensorManager.updateAmmoniaWarmup();
    unsigned long now = millis();

    latestReadings = sensorManager.readAll(fanController.isActive(), fanController.getDutyPercent());
    fanController.update(latestReadings.ambient_temp_c, latestReadings.ambientTempValid);

    // Stage 3: "Connected" - reacts to a phone ACTUALLY joining our
    // AP (WiFi.softAPgetStationNum() > 0), not just the AP existing.
    // Also reacts to the phone leaving again, going back to the
    // blinking "waiting" state rather than staying falsely green.
    bool phoneConnectedNow = WiFi.softAPgetStationNum() > 0;
    if (phoneConnectedNow != phoneWasConnected) {
        phoneWasConnected = phoneConnectedNow;
        if (phoneConnectedNow) {
            Serial.println(F("[WiFi] Phone connected"));
            lcdDashboard.showRaw("Connected", "");
            statusIndicator.setState(WifiLedState::GREEN);
            statusIndicator.beepSuccess();
        } else {
            Serial.println(F("[WiFi] Phone disconnected - waiting again"));
            lcdDashboard.showRaw("Connecting to", "WiFi");
        }
    }
    if (!phoneConnectedNow) {
        statusIndicator.blinkYellow(now);
    }

    // Broadcasting our own AP always succeeds and stays up on its
    // own - there's no "reconnect" concept the way joining someone
    // else's router had. GREEN simply means the device finished
    // booting and its local server is running.
    if (localApiServer) localApiServer->handleClient();

    if (now - lastSensorTime >= INTERVAL_SENSOR_MS) {
        lastSensorTime = now;
        sensorManager.printReadings(latestReadings);
    }
    if (now - lastHealthCheck >= HEALTH_CHECK_MS) {
        lastHealthCheck = now;
        runHealthCheck(latestReadings);
        lcdDashboard.showDashboard(countWorking(), TOTAL_COMPONENTS);
    }
}

/**
 * Every slot reflects an ACTUAL, independently-checked state - no
 * slot is hardcoded true "because it should be working."
 */
void runHealthCheck(const SensorReadings& r) {
    componentOK[0] = r.phValid;
    componentOK[1] = r.turbidityValid;
    componentOK[2] = r.ammoniaValid;
    componentOK[3] = r.waterTempValid;
    componentOK[4] = r.ambientTempValid;
    componentOK[5] = lcdDashboard.isResponding();
    componentOK[6] = fanController.isActive();
    componentOK[7] = feedingMechanism.isServoAttached();
}

int countWorking() {
    int count = 0;
    for (int i = 0; i < TOTAL_COMPONENTS; i++) if (componentOK[i]) count++;
    return count;
}
