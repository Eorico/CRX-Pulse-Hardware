#include <WiFi.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>


// =====================================================
// WIFI
// =====================================================

const char* ssid = "HUAWEI-ZwK3";
const char* password = "8d4fXku3";

#define WIFI_LED_PIN 2

bool lastWiFiState = false;

unsigned long lastWiFiAttempt = 0;
const unsigned long WIFI_RETRY_INTERVAL = 10000;


// =====================================================
// PIN DEFINITIONS
// =====================================================

// DHT11
#define DHT_PIN 16
#define DHT_TYPE DHT11

// DS18B20 Water Temperature
#define WATER_TEMP_PIN 18

// Analog Sensors
#define TURBIDITY_PIN 34
#define PH_PIN 32
#define AMMONIA_PIN 35

// Servo
#define SERVO_PIN 19

// I2C LCD
#define I2C_SDA 21
#define I2C_SCL 22


// =====================================================
// L298N MOTOR 1
// INDEPENDENT / MANUAL CONTROL
// =====================================================

#define MOTOR1_EN 25
#define MOTOR1_IN1 26
#define MOTOR1_IN2 27


// =====================================================
// L298N MOTOR 2
// DHT11 AUTOMATIC COOLING
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

// Servo calibration constants
//
// Target grams | Delay (ms)
// -------------+-----------
//      5       |    400
//     10       |    800
//     15       |   1200
//     20       |   1600
//     25       |   2000
//     30       |   2400

const int SERVO_OPEN_ANGLE = 160;  // degrees

const unsigned long SERVO_5_GRAMS  =  400;  // ms
const unsigned long SERVO_10_GRAMS =  800;
const unsigned long SERVO_15_GRAMS = 1200;  
const unsigned long SERVO_20_GRAMS = 1600;
const unsigned long SERVO_25_GRAMS = 2000;  
const unsigned long SERVO_30_GRAMS = 2400;


// =====================================================
// TURBIDITY CALIBRATION
// =====================================================

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
// reading on a threshold lands in the band named at that row.
const int   TURBIDITY_POINTS = 5;
const float TURBIDITY_V[TURBIDITY_POINTS] = {
  1.36460, 1.33015, 1.28065, 1.22865, 1.19160
};
const float TURBIDITY_PCT[TURBIDITY_POINTS] = {
  0.0, 19.9, 48.5, 78.6, 100.0
};

const float V_CLEAR = 1.36460;  //   0.00% relative turbidity
const float V_HIGH  = 1.19160;  // 100.00% relative turbidity

// Each cut is the TOP of its band, so equality stays in the band the
// standard names: 19.9 is Clear, 48.5 is Slightly, 78.6 is Turbid.
const float TURBIDITY_CLEAR_MAX    = 19.9;
const float TURBIDITY_SLIGHTLY_MAX = 48.5;
const float TURBIDITY_TURBID_MAX   = 78.6;

// Relative turbidity % for a pin voltage, interpolated along the table.
float turbidityPercentOf(float volts) {
  if (volts >= TURBIDITY_V[0])                    return 0.0;
  if (volts <= TURBIDITY_V[TURBIDITY_POINTS - 1]) return 100.0;
  for (int i = 0; i < TURBIDITY_POINTS - 1; i++) {
    const float vHi = TURBIDITY_V[i];
    const float vLo = TURBIDITY_V[i + 1];
    if (volts <= vHi && volts >= vLo) {
      return TURBIDITY_PCT[i] +
             (vHi - volts) * (TURBIDITY_PCT[i + 1] - TURBIDITY_PCT[i]) / (vHi - vLo);
    }
  }
  return 100.0;
}

String turbidityStatusOf(float percent) {
  if (percent <= TURBIDITY_CLEAR_MAX)    return "Clear";
  if (percent <= TURBIDITY_SLIGHTLY_MAX) return "Slightly Turbid";
  if (percent <= TURBIDITY_TURBID_MAX)   return "Turbid";
  return "Highly Turbid";
}


// =====================================================
// pH CALIBRATION
// =====================================================

// Explicit standard buffers, not a formula that assumes 7.00 / 4.00:
//
//   buffer    true pH   probe volts
//   neutral      6.86      1.950
//   acid         4.01      2.447
//   base         9.18      1.425
//
// Probe voltages are the RECONSTRUCTED output, i.e. after
// PH_DIVIDER_RATIO. The production firmware applies a median/EMA filter
// before calling this curve; the values here are the calibration anchors.
const float PH_NEUTRAL = 6.86;
const float PH_ACID    = 4.01;
const float PH_BASE    = 9.18;

const float voltage_pH7 = 1.950;  // 6.86 buffer
const float voltage_pH4 = 2.447;  // 4.01 buffer
const float voltage_pH9 = 1.425;  // 9.18 buffer

const float PH_DIVIDER_RATIO = 2.0;

// Two segments, not one line: the electrode is about 174 mV per pH on
// the acid side against 226 mV per pH on the base side. A single
// straight fit would miss the outer buffers.
float phValueOf(float probeVolts) {
  float value;
  if (probeVolts >= voltage_pH7) {
    value = PH_NEUTRAL + (probeVolts - voltage_pH7) *
            (PH_ACID - PH_NEUTRAL) / (voltage_pH4 - voltage_pH7);
  } else {
    value = PH_NEUTRAL + (probeVolts - voltage_pH7) *
            (PH_BASE - PH_NEUTRAL) / (voltage_pH9 - voltage_pH7);
  }
  return constrain(value, 0.0, 14.0);
}


// =====================================================
// MQ-137
// =====================================================
//
// MQ-137 AO
//     |
//    10k
//     |
//     +------ GPIO35
//     |
//    10k
//     |
//    GND
//
// GPIO35 sees half the MQ-137 output voltage.
//
// Example:
// MQ-137 AO = 4.0 V
// GPIO35     = 2.0 V
//
// =====================================================

const float AMMONIA_DIVIDER_RATIO = 2.0;

int ammoniaADC = 0;
float ammoniaADCVoltage = 0.0;
float ammoniaSensorVoltage = 0.0;

float ammoniaEstimatedMgL = 0.0;
bool ammoniaInCalibrationRange = false;
bool ammoniaBelowRange = false;   // reading bottomed out below V0
bool ammoniaAboveRange = false;   // reading past V2, mg/L held at C2

// Turbidity raw values, promoted to globals so
// printSensorData() can display them beside clarity.

int turbidityADC = 0;
float turbidityADCVoltage = 0.0;


// =====================================================
// VARIABLES
// =====================================================

float waterTemperature = 0.0;

float airTemperature = 0.0;
float humidity = 0.0;

// NEW
float relativeTurbidity = 0.0;
String turbidityStatus = "--";

float phVoltage = 0.0;
float pH = 7.0;

int servoPosition = 0;

int motor1PWM = 0;
int motor2PWM = 0;


// =====================================================
// LCD
// =====================================================

int lcdPage = 0;

unsigned long lastLCDUpdate = 0;

const unsigned long LCD_INTERVAL = 2500;


// =====================================================
// SENSOR TIMING
// =====================================================

unsigned long lastSensorRead = 0;

const unsigned long SENSOR_INTERVAL = 2000;


// =====================================================
// COOLING TEMPERATURE SETTINGS
// =====================================================
//
// Below 25°C:
//      OFF
//
// 25°C:
//      ~9V-equivalent PWM
//
// 25–40°C:
//      9V → 12V-equivalent PWM
//
// 40°C+:
//      100%
//
// =====================================================

const float COOLING_START = 25.0;
const float COOLING_MAX = 40.0;

const int COOLING_MIN_PWM = 191;
const int COOLING_MAX_PWM = 255;


// =====================================================
// WIFI START
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


// =====================================================
// CHECK WIFI
// =====================================================

void checkWiFi() {

  bool connected =
    (WiFi.status() == WL_CONNECTED);


  // ---------------------------------------------------
  // CONNECTED
  // ---------------------------------------------------

  if (connected) {

    digitalWrite(WIFI_LED_PIN, HIGH);

    if (!lastWiFiState) {

      lastWiFiState = true;

      Serial.println();
      Serial.println("WiFi: connected");

      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());

      Serial.print("Signal strength: ");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm");
    }

    return;
  }


  // ---------------------------------------------------
  // DISCONNECTED
  // ---------------------------------------------------

  digitalWrite(WIFI_LED_PIN, LOW);

  if (lastWiFiState) {

    lastWiFiState = false;

    Serial.println();
    Serial.println("WiFi: disconnected");
  }


  // ---------------------------------------------------
  // RETRY
  // ---------------------------------------------------

  if (millis() - lastWiFiAttempt >=
      WIFI_RETRY_INTERVAL) {

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

void readSensors() {

  // ---------------------------------------------------
  // DS18B20
  // ---------------------------------------------------

  waterTempSensor.requestTemperatures();

  waterTemperature =
    waterTempSensor.getTempCByIndex(0);


  // ---------------------------------------------------
  // DHT11
  // ---------------------------------------------------

  float newHumidity =
    dht.readHumidity();

  float newAirTemperature =
    dht.readTemperature();


  if (!isnan(newHumidity) &&
      !isnan(newAirTemperature)) {

    humidity =
      newHumidity;

    airTemperature =
      newAirTemperature;
  }


  // ---------------------------------------------------
  // TURBIDITY
  // ---------------------------------------------------

  turbidityADC =
    analogRead(TURBIDITY_PIN);

  turbidityADCVoltage =
    (turbidityADC / 4095.0) * 3.3;

  // Piecewise curve, then the band from the PERCENTAGE -- so the
  // status and the number can never disagree.
  relativeTurbidity =
    turbidityPercentOf(turbidityADCVoltage);

  turbidityStatus =
    turbidityStatusOf(relativeTurbidity);

  // ---------------------------------------------------
  // pH
  // ---------------------------------------------------

  int rawPH =
    analogRead(PH_PIN);

  float phADCVoltage =
    (rawPH / 4095.0) * 3.3;

  phVoltage =
    phADCVoltage *
    PH_DIVIDER_RATIO;


  pH =
    phValueOf(phVoltage);


  // ---------------------------------------------------
  // MQ-137 AMMONIA
  // ---------------------------------------------------

  ammoniaADC =
    analogRead(AMMONIA_PIN);


  // Voltage measured by ESP32
  ammoniaADCVoltage =
    (ammoniaADC / 4095.0) * 3.3;


  // Reconstruct original MQ-137 AO voltage
  ammoniaSensorVoltage =
    ammoniaADCVoltage *
    AMMONIA_DIVIDER_RATIO;

    // PROVISIONAL NH4/NH3 ESTIMATE (mg/L)
  // Based on recorded Sera reference concentrations.
  // Not yet validated.

  const float V0 = 0.2655;
  const float V1 = 0.4030; 
  const float V2 = 0.4400;

  const float C0 = 0.00;
  const float C1 = 0.17;
  const float C2 = 0.53;

  // Outside the calibrated span the reading is 0.00, at BOTH ends.
  //
  //   below V0 -> 0.00: the honest floor.
  //   above V2 -> 0.00 too. This used to hold at C2 (0.53), on the
  //     reasoning that past the top reference the true concentration
  //     must be higher still. That only holds while the calibration
  //     fits the sensor, and on this unit it does not: with nothing to
  //     detect it sits near 0.629 V, ~0.19 V above the top reference,
  //     so the tank read "0.53 Critical" permanently.
  //
  // Cost: while out of band, a genuinely high level also reads 0.00.
  // Only recalibration fixes that; the voltage below is the one
  // trustworthy figure in this state.
  ammoniaInCalibrationRange =
      (ammoniaSensorVoltage >= V0 && ammoniaSensorVoltage <= V2);

  ammoniaBelowRange = (ammoniaSensorVoltage <  V0);
  ammoniaAboveRange = (ammoniaSensorVoltage >  V2);

  if (ammoniaSensorVoltage <= V0) {
    ammoniaEstimatedMgL = C0;
  }
  else if (ammoniaSensorVoltage >  V2) {
    // strictly above, so V2 itself still yields C2 as calibrated
    ammoniaEstimatedMgL = C0;
  }
  else if (ammoniaSensorVoltage <= V1) {
    ammoniaEstimatedMgL =
        C0 +
        (ammoniaSensorVoltage - V0) *
        (C1 - C0) / (V1 - V0);
  }
  else {
    ammoniaEstimatedMgL =
        C1 +
        (ammoniaSensorVoltage - V1) *
        (C2 - C1) / (V2 - V1);
  }
}


// =====================================================
// MOTOR 2
// AUTOMATIC DHT11 COOLING
// =====================================================

void controlCoolingMotor() {

  int pwm = 0;


  if (airTemperature < COOLING_START) {

    pwm = 0;
  }


  else if (airTemperature < COOLING_MAX) {

    pwm =
      map(
        airTemperature * 10,
        COOLING_START * 10,
        COOLING_MAX * 10,
        COOLING_MIN_PWM,
        COOLING_MAX_PWM
      );
  }


  else {

    pwm = COOLING_MAX_PWM;
  }


  pwm =
    constrain(
      pwm,
      0,
      255
    );


  motor2PWM = pwm;


  analogWrite(
    MOTOR2_EN,
    motor2PWM
  );
}


// =====================================================
// MOTOR 1
// INDEPENDENT CONTROL
// =====================================================

void setMotor1(int pwm) {

  motor1PWM =
    constrain(
      pwm,
      0,
      255
    );


  analogWrite(
    MOTOR1_EN,
    motor1PWM
  );


  Serial.print("Motor 1 PWM: ");
  Serial.print(motor1PWM);

  Serial.print(" / 255   ");

  Serial.print(
    (motor1PWM / 255.0) * 100.0,
    0
  );

  Serial.println("%");
}


// =====================================================
// SERIAL COMMANDS
// =====================================================

void checkSerialCommand() {

  if (!Serial.available()) {
    return;
  }


  char command =
    Serial.read();


  // ---------------------------------------------------
  // SERVO
  // ---------------------------------------------------


  if (command == '1') {

    myServo.write(SERVO_OPEN_ANGLE);

    servoPosition = SERVO_OPEN_ANGLE;

    Serial.println(
      "Servo -> OPEN 160 degrees"  
    );

    delay(SERVO_5_GRAMS);

    myServo.write(180);

    servoPosition = 180;

    Serial.println( 
      "Servo -> CLOSE 180 degrees"
    );
  }


  // ---------------------------------------------------
  // MOTOR 1
  // ---------------------------------------------------

  else if (command == '2') {

    setMotor1(0);

    Serial.println(
      "Motor 1 -> OFF"
    );
  }


  else if (command == '3') {

    setMotor1(90);

    Serial.println(
      "Motor 1 -> 25%"
    );
  }


  else if (command == '4') {

    setMotor1(128);

    Serial.println(
      "Motor 1 -> 50%"
    );
  }


  else if (command == '5') {

    setMotor1(191);

    Serial.println(
      "Motor 1 -> 75%"
    );
  }


  else if (command == '6') {

    setMotor1(255);

    Serial.println(
      "Motor 1 -> 100%"
    );
  }

  else if (command == '7') {

    myServo.write(SERVO_OPEN_ANGLE);

    servoPosition = SERVO_OPEN_ANGLE;

    Serial.println(
      "Servo -> OPEN 160 degrees (10g)" 
    );

    delay(SERVO_10_GRAMS);

    myServo.write(180);

    servoPosition = 180;

    Serial.println(
      "Servo -> CLOSE 180 degrees"
    );
  }

   else if (command == '8') {

    myServo.write(SERVO_OPEN_ANGLE);

    servoPosition = SERVO_OPEN_ANGLE;

    Serial.println(
      "Servo -> OPEN 160 degrees (15g)"
    ); 
    delay(SERVO_15_GRAMS);

    myServo.write(180);

    servoPosition = 180;

    Serial.println( 
      "Servo -> CLOSE 180 degrees"
    );
  }

  else if (command == '9') {
    myServo.write(SERVO_OPEN_ANGLE);

    servoPosition = SERVO_OPEN_ANGLE;

    Serial.println(
      "Servo -> OPEN 160 degrees (20g)"  
    );

    delay(SERVO_20_GRAMS);

    myServo.write(180);

    servoPosition = 180;

    Serial.println(
      "Servo -> CLOSE 180 degrees" 
    );
  }

  else if (command == '10') {
     myServo.write(SERVO_OPEN_ANGLE);

    servoPosition = SERVO_OPEN_ANGLE;

    Serial.println(
      "Servo -> OPEN 160 degrees (25g)"
    );

    delay(SERVO_25_GRAMS);

    myServo.write(180); 

    servoPosition = 180;

    Serial.println(
      "Servo -> CLOSE 180 degrees"
    );  
  }

  else if (command == '11') {
    myServo.write(SERVO_OPEN_ANGLE);

    servoPosition = SERVO_OPEN_ANGLE;

    Serial.println(
      "Servo -> OPEN 160 degrees (30g)"
    );

    delay(SERVO_30_GRAMS);

    myServo.write(180);

    servoPosition = 180;

    Serial.println(
      "Servo -> CLOSE 180 degrees"  
    );
  }

}




// =====================================================
// AMMONIA PERCENT + QUALITY
// =====================================================
//
// ADDED - does not modify any existing logic. Reads the
// ammoniaSensorVoltage that readSensors() already computes.
//
// The "%" is position across the MQ-137's own output span,
// NOT absolute ppm. An MQ-137 cannot report real ppm without
// calibration against a known reference gas, so treat this as
// a relative indicator: good for "is it rising / is it safe",
// not for a lab-grade number.
//
// AMMONIA_V_CLEAN / AMMONIA_V_FOUL are placeholders. To set
// them properly: note ammoniaSensorVoltage in clean air after
// the ~60s heater warmup (that is your CLEAN value), then note
// it again over a known-foul sample (that is your FOUL value).
//
// =====================================================

const float AMMONIA_V_CLEAN = 1.00;   // sensor volts in clean air
const float AMMONIA_V_FOUL  = 4.00;   // sensor volts at high NH3

int ammoniaPercent = 0;


int getAmmoniaPercent() {

  float pct =
    ((ammoniaSensorVoltage - AMMONIA_V_CLEAN) /
     (AMMONIA_V_FOUL - AMMONIA_V_CLEAN)) * 100.0;

  return constrain((int)pct, 0, 100);
}


String getAmmoniaStatus(float mgL) {

  if (mgL < 0.25) {
    return "Safe";
  }

  if (mgL < 0.50) {
    return "Warning";
  }

  return "Critical";
}


// =====================================================
// SERIAL SENSOR MONITOR
// =====================================================

void printSensorData() {

  Serial.println();
  Serial.println(
    "========== SENSOR DATA =========="
  );


  Serial.print("Water Temp: ");
  Serial.print(waterTemperature);
  Serial.println(" C");


  Serial.print("Air Temp:   ");
  Serial.print(airTemperature);
  Serial.println(" C");


  Serial.print("Humidity:   ");
  Serial.print(humidity);
  Serial.println(" %");


  // NEW
  Serial.print("Turbidity:  ");
  Serial.print(relativeTurbidity, 2);
  Serial.print("% (");
  Serial.print(turbidityStatus);
  Serial.print(")   |   ADC: ");
  Serial.print(turbidityADC);
  Serial.print("   ADC V: ");
  Serial.print(turbidityADCVoltage, 3);
  Serial.println(" V");


  Serial.print("pH:         ");
  Serial.println(pH, 2);


  Serial.print("pH Voltage: ");
  Serial.print(phVoltage, 3);
  Serial.println(" V");


  // ---------------------------------------------------
  // MQ-137
  // ---------------------------------------------------

    // Raw sensor lines retained.
  Serial.print("NH3 ADC: ");
  Serial.print(ammoniaADC);

  Serial.print("   ADC V: ");
  Serial.print(ammoniaADCVoltage, 3);
  Serial.print(" V");

  Serial.print("   Sensor: ");
  Serial.print(ammoniaSensorVoltage, 3);
  Serial.println(" V");


  // Estimated concentration block.
  Serial.println("Estimated ammonia concentration");

  Serial.print(ammoniaEstimatedMgL, 2);
  Serial.print(" mg/L   (");
  Serial.print(ammoniaSensorVoltage, 3);
  Serial.println(" V)");

  if (ammoniaBelowRange) {
    Serial.print("  at floor: ");
    Serial.print(ammoniaSensorVoltage, 3);
    Serial.print(" V is below the ");
    Serial.print(0.2655, 3);
    Serial.println(" V zero reference");
  }
  else if (ammoniaAboveRange) {
    Serial.print("  OUT OF BAND (high): ");
    Serial.print(ammoniaSensorVoltage, 3);
    Serial.print(" V is past the ");
    Serial.print(0.4400, 3);
    Serial.println(" V top reference - forced to 0.00, NOT proof of clean water");
  }

  Serial.println("NH4/NH3 estimate");

  Serial.println();

  Serial.println("Status");

  // Always a status now, from the clamped figure.
  Serial.println(getAmmoniaStatus(ammoniaEstimatedMgL));


  // ---------------------------------------------------
  // MOTORS
  // ---------------------------------------------------

  Serial.print("Motor 1:    ");
  Serial.print(motor1PWM);
  Serial.println(" / 255");


  Serial.print("Cooling:    ");
  Serial.print(motor2PWM);
  Serial.println(" / 255");


  Serial.println(
    "================================="
  );
}


// =====================================================
// LCD DISPLAY
// =====================================================

void updateLCD() {

  lcd.clear();


  switch (lcdPage) {


    // -------------------------------------------------
    // PAGE 0
    // WATER TEMP + pH
    // -------------------------------------------------

    case 0:

      lcd.setCursor(0, 0);

      lcd.print("Water:");

      lcd.print(
        waterTemperature,
        1
      );

      lcd.print((char)223);
      lcd.print("C");


      lcd.setCursor(0, 1);

      lcd.print("pH:");

      lcd.print(
        pH,
        2
      );

      break;


    // -------------------------------------------------
    // PAGE 1
    // AIR TEMP + HUMIDITY
    // -------------------------------------------------

    case 1:

      lcd.setCursor(0, 0);

      lcd.print("Air:");

      lcd.print(
        airTemperature,
        1
      );

      lcd.print((char)223);
      lcd.print("C");


      lcd.setCursor(0, 1);

      lcd.print("Hum:");

      lcd.print(
        humidity,
        1
      );

      lcd.print("%");

      break;


    // -------------------------------------------------
    // PAGE 2
    // WATER CLARITY
    // -------------------------------------------------

    case 2:

      // NEW
      lcd.print(turbidityStatus);

      lcd.setCursor(0, 1);
      lcd.print(relativeTurbidity, 1);
      lcd.print("% turbid");

      break;


    // -------------------------------------------------
    // PAGE 3
    // AMMONIA
    // -------------------------------------------------

    case 3:

      lcd.setCursor(0, 0);

      lcd.print("NH3 Sensor");


      lcd.setCursor(0, 1);

      lcd.print(
        ammoniaSensorVoltage,
        2
      );

      lcd.print("V");

      break;


    // -------------------------------------------------
    // PAGE 4
    // MOTOR STATUS
    // -------------------------------------------------

    case 4:

      lcd.setCursor(0, 0);

      lcd.print("M1:");

      lcd.print(
        (motor1PWM / 255.0) * 100.0,
        0
      );

      lcd.print("%");


      lcd.setCursor(0, 1);

      lcd.print("Cool:");

      lcd.print(
        (motor2PWM / 255.0) * 100.0,
        0
      );

      lcd.print("%");

      break;
  }


  lcdPage++;


  if (lcdPage > 4) {

    lcdPage = 0;
  }
}


// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  delay(1000);


  // ===================================================
  // WIFI LED
  // ===================================================

  pinMode(
    WIFI_LED_PIN,
    OUTPUT
  );

  digitalWrite(
    WIFI_LED_PIN,
    LOW
  );


  // ===================================================
  // WIFI
  // ===================================================

  startWiFi();


  // ===================================================
  // SYSTEM
  // ===================================================

  Serial.println();

  Serial.println(
    "================================"
  );

  Serial.println(
    "CRX-PULSE CONTROL SYSTEM"
  );

  Serial.println(
    "================================"
  );


  // ===================================================
  // ADC
  // ===================================================

  analogReadResolution(12);

  analogSetPinAttenuation(
    TURBIDITY_PIN,
    ADC_11db
  );

  analogSetPinAttenuation(
    PH_PIN,
    ADC_11db
  );

  analogSetPinAttenuation(
    AMMONIA_PIN,
    ADC_11db
  );


  // ===================================================
  // DHT11
  // ===================================================

  dht.begin();


  // ===================================================
  // DS18B20
  // ===================================================

  waterTempSensor.begin();


  // ===================================================
  // LCD
  // ===================================================

  Wire.begin(
    I2C_SDA,
    I2C_SCL
  );

  Wire.setClock(100000);

  delay(100);

  lcd.init();

  delay(100);

  lcd.backlight();

  delay(100);

  lcd.clear();

  delay(50);

  lcd.setCursor(0, 0);
  lcd.print("CRX-PULSE *_*");

  lcd.setCursor(0, 1);
  lcd.print("System Starting");

  delay(2000);

  lcd.clear();


  // ===================================================
  // SERVO
  // ===================================================

   // 180 = CLOSED / default resting position
  // 150 = OPEN (feeding)
  //
  // Reserve an LEDC timer before attaching, so the two
  // analogWrite() motors set up further below cannot take
  // the timer the servo needs.

  ESP32PWM::allocateTimer(0);

  myServo.setPeriodHertz(50);

  // Explicit MG946R pulse range. The library default is
  // narrower and can stop the horn reaching full travel.

  myServo.attach(
    SERVO_PIN,
    500,
    2400
  );

  // First pulse the servo ever receives commands 180, so
  // it settles at CLOSED rather than sweeping from 0.

  myServo.write(180);

  servoPosition = 180;


  // ===================================================
  // MOTOR 1
  // ===================================================

  pinMode(
    MOTOR1_EN,
    OUTPUT
  );

  pinMode(
    MOTOR1_IN1,
    OUTPUT
  );

  pinMode(
    MOTOR1_IN2,
    OUTPUT
  );


  digitalWrite(
    MOTOR1_IN1,
    LOW
  );

  digitalWrite(
    MOTOR1_IN2,
    HIGH
  );

  analogWrite(
    MOTOR1_EN,
    0
  );


  // ===================================================
  // MOTOR 2
  // ===================================================

  pinMode(
    MOTOR2_EN,
    OUTPUT
  );

  pinMode(
    MOTOR2_IN1,
    OUTPUT
  );

  pinMode(
    MOTOR2_IN2,
    OUTPUT
  );


  digitalWrite(
    MOTOR2_IN1,
    HIGH
  );

  digitalWrite(
    MOTOR2_IN2,
    LOW
  );

  analogWrite(
    MOTOR2_EN,
    0
  );


  // ===================================================
  // SERIAL COMMAND GUIDE
  // ===================================================

  Serial.println();
  Serial.println("SERIAL COMMANDS");
  Serial.println("----------------------------");

  Serial.println("0 = Servo OPEN 160 degrees (5g)"); 
  Serial.println("1 = Servo CLOSE 180 degrees");
  Serial.println("7 = Servo OPEN 160 degrees (5g)");
  Serial.println("8 = Servo OPEN 160 degrees (10g)");
  Serial.println("9 = Servo OPEN 160 degrees (15g)");
  Serial.println("a = Servo OPEN 160 degrees (20g)");
  Serial.println("b = Servo OPEN 160 degrees (25g)");
  Serial.println("c = Servo OPEN 160 degrees (30g)");

  Serial.println();
  Serial.println("MOTOR 1:");

  Serial.println("2 = Motor 1 OFF");
  Serial.println("3 = Motor 1 25%");
  Serial.println("4 = Motor 1 50%");
  Serial.println("5 = Motor 1 75%");
  Serial.println("6 = Motor 1 100%");

  Serial.println();
  Serial.println("Motor 2 = DHT11 AUTOMATIC");

  Serial.println();
  Serial.println("MQ-137 = GPIO35 ANALOG");

  Serial.println("----------------------------");
}


// =====================================================
// MAIN LOOP
// =====================================================

void loop() {

  // ---------------------------------------------------
  // WIFI
  // ---------------------------------------------------

  checkWiFi();


  // ---------------------------------------------------
  // SERIAL COMMANDS
  // ---------------------------------------------------
  // How long the feeder door stays open before auto-closing.
// Raise this if not enough feed drops; lower it for less.

  checkSerialCommand();


  // ---------------------------------------------------
  // SENSOR UPDATE
  // ---------------------------------------------------

  if (
    millis() - lastSensorRead >=
    SENSOR_INTERVAL
  ) {

    lastSensorRead =
      millis();


    readSensors();


    // Motor 2 controlled
    // by DHT11 temperature

    controlCoolingMotor();


    printSensorData();
  }


  // ---------------------------------------------------
  // LCD UPDATE
  // ---------------------------------------------------

  if (
    millis() - lastLCDUpdate >=
    LCD_INTERVAL
  ) {

    lastLCDUpdate =
      millis();


    updateLCD();
  }
}