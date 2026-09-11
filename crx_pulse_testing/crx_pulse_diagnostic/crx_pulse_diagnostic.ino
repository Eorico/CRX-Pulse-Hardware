/*
 * CRX-PULSE HARDWARE DIAGNOSTIC SKETCH (interactive, menu-driven)
 * =================================================================
 * Standalone test sketch - NOT the main firmware. Flash this
 * temporarily to test each component individually and in isolation.
 *
 * Open Serial Monitor at 115200 baud, line ending set to "Newline",
 * after flashing. Type the letter for the test you want and press
 * Enter - the menu reappears automatically after each test.
 *
 * Shows RAW values regardless of any pass/fail threshold - the point
 * is to see exactly what each component is doing, so a wiring fault
 * is visible directly rather than hidden behind a decision.
 *
 * No water level sensor - not present on this hardware, not tested.
 */

#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>
#include <WiFi.h>

// ── Same pins as the main firmware ──
#define PIN_PH              35
#define PIN_TURBIDITY       34
#define PIN_AMMONIA         39
#define PIN_TEMP_SENSOR      4

#define PIN_LCD_SDA         21
#define PIN_LCD_SCL         22
#define LCD_I2C_ADDR      0x27

#define PIN_LED_RED         17
#define PIN_LED_YELLOW       5
#define PIN_LED_GREEN       23
#define PIN_PIEZZO          16

#define PIN_SERVO           33
#define PIN_FAN_ENB         32

#define ADC_MAX_VALUE     4095.0f
#define ADC_REF_VOLTAGE      3.3f

OneWire oneWire(PIN_TEMP_SENSOR);
DallasTemperature tempSensors(&oneWire);
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, 16, 2);
Servo feedServo;

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println(F("========================================"));
    Serial.println(F("  CRX-PULSE HARDWARE DIAGNOSTIC (interactive)"));
    Serial.println(F("========================================"));
    printMenu();
}

void loop() {
    if (!Serial.available()) return;

    char choice = Serial.read();
    // Consume any trailing newline/carriage-return so it doesn't get
    // misread as a second, unintended menu selection on the next pass.
    while (Serial.available() && (Serial.peek() == '\n' || Serial.peek() == '\r')) {
        Serial.read();
    }

    runTest(toupper(choice));
    printMenu();
}

void printMenu() {
    Serial.println();
    Serial.println(F("---------- SELECT A TEST ----------"));
    Serial.println(F("A) pH sensor"));
    Serial.println(F("B) Ammonia sensor (MQ-137)"));
    Serial.println(F("C) Turbidity sensor"));
    Serial.println(F("D) Water temperature (DS18B20)"));
    Serial.println(F("E) Housing temperature (DS18B20)"));
    Serial.println(F("F) LCD display"));
    Serial.println(F("G) Status LEDs (Red/Yellow/Green)"));
    Serial.println(F("H) Piezo buzzer"));
    Serial.println(F("I) Fan (DC motor, PWM)"));
    Serial.println(F("J) Feeding servo motor"));
    Serial.println(F("K) WiFi (local AP / router connection)"));
    Serial.println(F("------------------------------------"));
    Serial.print(F("Enter choice: "));
}

void runTest(char choice) {
    Serial.println();
    switch (choice) {
        case 'A': testPH(); break;
        case 'B': testAmmonia(); break;
        case 'C': testTurbidity(); break;
        case 'D': testWaterTemp(); break;
        case 'E': testHousingTemp(); break;
        case 'F': testLCD(); break;
        case 'G': testLEDs(); break;
        case 'H': testPiezo(); break;
        case 'I': testFan(); break;
        case 'J': testServo(); break;
        case 'K': testWiFiMenu(); break;
        default:
            Serial.println(F("Not a recognized option - pick a letter from the menu."));
    }
}

// Runs a sensor read loop for a fixed window, printing live values -
// shared by every analog/temperature test so each one behaves the
// same way and prints the same "press any key to stop early" hint.
void liveReadLoop(const char* title, unsigned long durationMs, unsigned long intervalMs, void (*readAndPrint)()) {
    Serial.print(F("--- "));
    Serial.print(title);
    Serial.println(F(" --- (press any key to return to menu early)"));
    unsigned long start = millis();
    unsigned long lastRead = 0;
    while (millis() - start < durationMs) {
        if (Serial.available()) {
            Serial.read();
            break;
        }
        if (millis() - lastRead >= intervalMs) {
            lastRead = millis();
            readAndPrint();
        }
    }
    Serial.println(F("--- test complete ---"));
}

void testPH() {
    liveReadLoop("pH SENSOR (GPIO 35)", 30000, 1000, []() {
        int raw = analogRead(PIN_PH);
        float voltage = (raw / ADC_MAX_VALUE) * ADC_REF_VOLTAGE;
        Serial.print(F("raw=")); Serial.print(raw);
        Serial.print(F("  voltage=")); Serial.print(voltage, 3); Serial.println(F("V"));
        if (voltage < 0.05f || voltage > 3.25f) {
            Serial.println(F("  (outside plausible range - check power/signal wiring)"));
        }
    });
}

void testAmmonia() {
    liveReadLoop("AMMONIA SENSOR / MQ-137 (GPIO 39)", 30000, 1000, []() {
        int raw = analogRead(PIN_AMMONIA);
        float voltage = (raw / ADC_MAX_VALUE) * ADC_REF_VOLTAGE;
        Serial.print(F("raw=")); Serial.print(raw);
        Serial.print(F("  voltage=")); Serial.print(voltage, 3); Serial.println(F("V"));
        if (voltage < 0.05f || voltage > 3.25f) {
            Serial.println(F("  (outside plausible range - MQ sensors also need ~60s heater warmup before settling)"));
        }
    });
}

void testTurbidity() {
    liveReadLoop("TURBIDITY SENSOR (GPIO 34)", 30000, 1000, []() {
        int raw = analogRead(PIN_TURBIDITY);
        float voltage = (raw / ADC_MAX_VALUE) * ADC_REF_VOLTAGE;
        Serial.print(F("raw=")); Serial.print(raw);
        Serial.print(F("  voltage=")); Serial.print(voltage, 3); Serial.println(F("V"));
    });
}

void ensureTempSensorsBegun() {
    static bool begun = false;
    if (!begun) {
        tempSensors.begin();
        begun = true;
    }
}

void testWaterTemp() {
    ensureTempSensorsBegun();
    int count = tempSensors.getDeviceCount();
    Serial.print(F("DS18B20 devices found on bus: ")); Serial.println(count);
    if (count == 0) {
        Serial.println(F("No devices found - check data wire (GPIO 4), the 4.7k pull-up resistor, and probe power."));
        return;
    }
    liveReadLoop("WATER TEMP (probe index 0)", 20000, 1000, []() {
        tempSensors.requestTemperatures();
        float t = tempSensors.getTempCByIndex(0);
        if (t == DEVICE_DISCONNECTED_C) {
            Serial.println(F("DISCONNECTED"));
        } else {
            Serial.print(t, 2); Serial.println(F(" C"));
        }
    });
}

void testHousingTemp() {
    ensureTempSensorsBegun();
    int count = tempSensors.getDeviceCount();
    Serial.print(F("DS18B20 devices found on bus: ")); Serial.println(count);
    if (count < 2) {
        Serial.println(F("Only one (or zero) probe found - housing/ambient temp needs a SECOND probe on the same data wire."));
        return;
    }
    liveReadLoop("HOUSING TEMP (probe index 1)", 20000, 1000, []() {
        tempSensors.requestTemperatures();
        float t = tempSensors.getTempCByIndex(1);
        if (t == DEVICE_DISCONNECTED_C) {
            Serial.println(F("DISCONNECTED"));
        } else {
            Serial.print(t, 2); Serial.println(F(" C"));
        }
    });
}

void testLCD() {
    Serial.println(F("--- LCD DISPLAY (GPIO 21/22, I2C) ---"));
    Wire.begin(PIN_LCD_SDA, PIN_LCD_SCL);
    delay(50);
    Wire.beginTransmission(LCD_I2C_ADDR);
    uint8_t result = Wire.endTransmission();
    if (result != 0) {
        Serial.print(F("No I2C ACK at 0x27 (error ")); Serial.print(result); Serial.println(F(") - try 0x3F, or check SDA/SCL/power wiring."));
        return;
    }
    Serial.println(F("I2C ACK received - wiring is correct. Displaying test pattern..."));
    lcd.init();
    lcd.backlight();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("LCD TEST OK"));
    lcd.setCursor(0, 1);
    lcd.print(F("1234567890ABCDEF"));
    Serial.println(F("Check the physical screen now - if the ACK succeeded above but the screen looks garbled/shifted, the LCD panel itself is likely faulty."));
}

void testLEDs() {
    Serial.println(F("--- STATUS LEDs (Red=17, Yellow=5, Green=23) ---"));
    pinMode(PIN_LED_RED, OUTPUT);
    pinMode(PIN_LED_YELLOW, OUTPUT);
    pinMode(PIN_LED_GREEN, OUTPUT);

    Serial.println(F("Red ON"));
    digitalWrite(PIN_LED_RED, HIGH); digitalWrite(PIN_LED_YELLOW, LOW); digitalWrite(PIN_LED_GREEN, LOW);
    delay(1000);

    Serial.println(F("Yellow ON"));
    digitalWrite(PIN_LED_RED, LOW); digitalWrite(PIN_LED_YELLOW, HIGH); digitalWrite(PIN_LED_GREEN, LOW);
    delay(1000);

    Serial.println(F("Green ON"));
    digitalWrite(PIN_LED_RED, LOW); digitalWrite(PIN_LED_YELLOW, LOW); digitalWrite(PIN_LED_GREEN, HIGH);
    delay(1000);

    Serial.println(F("All OFF"));
    digitalWrite(PIN_LED_RED, LOW); digitalWrite(PIN_LED_YELLOW, LOW); digitalWrite(PIN_LED_GREEN, LOW);
    Serial.println(F("If any color didn't visibly light: check that LED's resistor and polarity (long leg = anode, toward the GPIO through the resistor)."));
}

void testPiezo() {
    Serial.println(F("--- PIEZO BUZZER (GPIO 16, PWM) ---"));
    ledcAttach(PIN_PIEZZO, 2000, 8);
    for (int i = 0; i < 3; i++) {
        Serial.println(F("beep"));
        ledcWrite(PIN_PIEZZO, 128);
        delay(200);
        ledcWrite(PIN_PIEZZO, 0);
        delay(200);
    }
    Serial.println(F("If you heard nothing: check polarity (some piezo buzzers are directional) and that GPIO 16 isn't shared/conflicting with another peripheral."));
}

void testFan() {
    Serial.println(F("--- FAN / DC MOTOR (GPIO 32, PWM) ---"));
    ledcAttach(PIN_FAN_ENB, 25000, 8);

    Serial.println(F("Ramping to 50% duty..."));
    ledcWrite(PIN_FAN_ENB, 128);
    delay(3000);

    Serial.println(F("Ramping to 100% duty..."));
    ledcWrite(PIN_FAN_ENB, 255);
    delay(3000);

    Serial.println(F("Stopping (0% duty)..."));
    ledcWrite(PIN_FAN_ENB, 0);
    Serial.println(F("If the fan never spun at all: check its power rail and the MOSFET/transistor driving it from GPIO 32 - a bare GPIO usually can't drive a fan's current directly."));
}

void testServo() {
    Serial.println(F("--- FEEDING SERVO (GPIO 33) ---"));
    feedServo.setPeriodHertz(50);
    feedServo.attach(PIN_SERVO, 500, 2400);

    Serial.println(F("Moving to 0 degrees..."));
    feedServo.write(0);
    delay(1000);

    Serial.println(F("Moving to 90 degrees..."));
    feedServo.write(90);
    delay(1000);

    Serial.println(F("Moving to 180 degrees..."));
    feedServo.write(180);
    delay(1000);

    Serial.println(F("Returning to 90 degrees..."));
    feedServo.write(90);
    delay(1000);

    feedServo.detach();
    Serial.println(F("If the servo didn't move (or jittered/strained): it likely isn't getting enough current from a shared 5V rail - feed it from a separate 5V supply with common ground."));
}

void testWiFiMenu() {
    Serial.println(F("--- WIFI TEST ---"));
    Serial.println(F("1) Local WiFi - broadcast the ESP32's own access point (matches production firmware)"));
    Serial.println(F("2) Router WiFi - connect to an existing WiFi network as a station (hardware/module test only - production firmware does not use this mode)"));
    Serial.print(F("Enter 1 or 2: "));

    while (!Serial.available()) { /* wait for input */ }
    char sub = Serial.read();
    while (Serial.available() && (Serial.peek() == '\n' || Serial.peek() == '\r')) Serial.read();
    Serial.println(sub);

    if (sub == '1') {
        testWiFiLocalAP();
    } else if (sub == '2') {
        testWiFiRouter();
    } else {
        Serial.println(F("Not 1 or 2 - returning to main menu."));
    }
}

void testWiFiLocalAP() {
    Serial.println(F("Starting local AP broadcast..."));
    WiFi.mode(WIFI_AP);
    bool ok = WiFi.softAP("CRX-Pulse-Diagnostic", "diagnostic123");
    if (!ok) {
        Serial.println(F("softAP() returned false - WiFi module may be faulty, or already in a bad state (try a fresh power cycle)."));
        return;
    }
    delay(500);
    Serial.print(F("AP broadcasting as \"CRX-Pulse-Diagnostic\" - IP address: "));
    Serial.println(WiFi.softAPIP());
    Serial.println(F("Check your phone's WiFi list now - you should see this network. Broadcasting for 20 seconds..."));
    delay(20000);
    WiFi.softAPdisconnect(true);
    Serial.println(F("AP stopped."));
}

void testWiFiRouter() {
    Serial.println(F("Enter your router's WiFi SSID, then press Enter:"));
    String ssid = readSerialLine();
    Serial.println(F("Enter the WiFi password, then press Enter:"));
    String password = readSerialLine();

    Serial.print(F("Connecting to \"")); Serial.print(ssid); Serial.println(F("\"..."));
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(500);
        Serial.print(F("."));
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print(F("Connected - IP address: "));
        Serial.println(WiFi.localIP());
        Serial.print(F("Signal strength (RSSI): "));
        Serial.print(WiFi.RSSI());
        Serial.println(F(" dBm"));
    } else {
        Serial.println(F("Failed to connect within 15 seconds - check the SSID/password, and that the router is a 2.4GHz network (the ESP32 cannot join 5GHz-only networks)."));
    }
    WiFi.disconnect(true);
}

String readSerialLine() {
    while (!Serial.available()) { /* wait */ }
    String line = Serial.readStringUntil('\n');
    line.trim();
    return line;
}
