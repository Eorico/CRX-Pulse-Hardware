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

const float V_CLEAR = 1.3646;   // 0.00% relative turbidity
const float V_HIGH  = 1.1916;   // 100.00% relative turbidity

const float TURBIDITY_T1 = 1.33015;  // Clear -> Slightly Turbid
const float TURBIDITY_T2 = 1.28065;  // Slightly Turbid -> Turbid
const float TURBIDITY_T3 = 1.22865;  // Turbid -> Highly Turbid


// =====================================================
// pH CALIBRATION
// =====================================================

// float voltage_pH7 = 1.501;
// float voltage_pH4 = 2.170;
// float voltage_pH9 = 1.042;   

// NEW — example, use YOUR readings
float voltage_pH7 = 1.950;   // whatever you got at 6.86
float voltage_pH4 = 2.447;   // whatever you got at 4.01
float voltage_pH9 = 1.425;   // whatever you got at 9.18

const float PH_DIVIDER_RATIO = 2.0;


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

  // NEW
  relativeTurbidity =
    ((V_CLEAR - turbidityADCVoltage) /
     (V_CLEAR - V_HIGH)) * 100.0;

  relativeTurbidity =
    constrain(
      relativeTurbidity,
      0.0,
      100.0
    );

  if (turbidityADCVoltage >= TURBIDITY_T1) {
    turbidityStatus = "Clear";
  }
  else if (turbidityADCVoltage >= TURBIDITY_T2) {
    turbidityStatus = "Slightly Turbid";
  }
  else if (turbidityADCVoltage >= TURBIDITY_T3) {
    turbidityStatus = "Turbid";
  }
  else {
    turbidityStatus = "Highly Turbid";
  }

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


  if (phVoltage >= voltage_pH7) {

    pH =
      7.0 +
      (phVoltage - voltage_pH7) *
      (4.0 - 7.0) /
      (voltage_pH4 - voltage_pH7);
  }

  else {

    pH =
      7.0 +
      (phVoltage - voltage_pH7) *
      (9.18 - 7.0) /
      (voltage_pH9 - voltage_pH7);
  }

  pH =
    constrain(
      pH,
      0.0,
      14.0
    );


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

  ammoniaInCalibrationRange = false;
  ammoniaEstimatedMgL = 0.0;

  if (ammoniaSensorVoltage >= V0 && 
      ammoniaSensorVoltage <= V2) {

    ammoniaInCalibrationRange = true;

    if (ammoniaSensorVoltage <= V1) {
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

  if (ammoniaInCalibrationRange) {
    Serial.print(ammoniaEstimatedMgL, 2);
    Serial.println(" mg/L");
  }
  else {
    Serial.println("-- mg/L (out of range)");
  }

  Serial.println("NH4/NH3 estimate");

  Serial.println();

  Serial.println("Status");

  if (ammoniaInCalibrationRange) {
    Serial.println(getAmmoniaStatus(ammoniaEstimatedMgL));
  }
  else {
    Serial.println("Unknown");
  }


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