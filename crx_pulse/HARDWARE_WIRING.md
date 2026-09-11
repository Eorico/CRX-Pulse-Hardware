# CRX-Pulse Hardware Wiring Guide

Firmware version referenced: `10.0.0`. All pin numbers below are exactly what's defined in `crx_pulse.ino` — if you rewire anything, update the corresponding `#define` in the firmware to match.

## Power Requirements

| Rail | Used by |
|---|---|
| 5V | LCD backlight, MQ-137 ammonia sensor heater, servo, fan |
| 3.3V | ESP32 logic, analog sensor signal reference |
| GND | Common ground — **every component below must share this ground with the ESP32**, even ones powered from 5V |

A shared, solid ground connection across every component is the single most common cause of "sensor not connected" symptoms when the wiring otherwise looks correct — worth double-checking first if multiple sensors fail at once.

## pH Sensor

| Wire | ESP32 Pin |
|---|---|
| Signal (analog out) | GPIO 35 |
| VCC | 5V (most analog pH boards regulate down internally) |
| GND | GND |

- GPIO 35 is an **ADC1, input-only** pin — correct for an analog read, but it cannot be repurposed as a digital output if you ever change the wiring.
- Expected voltage range for a connected, powered probe: roughly **0.05V–3.25V**. A reading stuck at 0.00V or exactly at 3.30V almost always means power or signal wiring, not a calibration issue.
- Calibration constants (`PH_VOLTAGE_AT_7`, `PH_VOLTS_PER_UNIT`) assume the probe has been calibrated in pH 7.0 buffer solution — do this before trusting absolute readings.

## Ammonia Sensor (MQ-137)

| Wire | ESP32 Pin |
|---|---|
| Signal (AOUT) | GPIO 39 |
| VCC (heater) | 5V |
| GND | GND |

- GPIO 39 is also ADC1, input-only.
- **MQ-series sensors need a genuine warm-up period** — the firmware enforces a 60-second warmup (`AMMONIA_WARMUP_MS`) before trusting a reading, and the sensor itself needs its internal heater to reach operating temperature. This is normal, not a fault.
- The heater draws more current than most other components on this board — if your 5V supply is shared and marginal, this is worth checking with a multimeter under load.

## Water Temperature + Housing Temperature (DS18B20 × 2)

| Wire | ESP32 Pin |
|---|---|
| Data (shared bus, both probes) | GPIO 4 |
| VCC (both probes) | 3.3V |
| GND (both probes) | GND |

- **Both probes share the same single data wire** (OneWire bus) — this is intentional, not a wiring mistake.
- **A 4.7kΩ pull-up resistor between the data wire and 3.3V is required** for reliable OneWire communication. Without it, the bus can appear to work intermittently or report zero devices — this is the single most common DS18B20 wiring mistake.
- The firmware distinguishes "water" vs "housing" by which probe is discovered first/second on the bus at boot, then locks onto each probe's unique ROM address for the rest of that power cycle. There's no way to guarantee which physical probe becomes "water" vs "housing" from a cold boot without either checking the diagnostic sketch's printed ROM addresses once and physically labeling each probe, or comparing which one reads a plausible water temperature at first boot.

## LCD Display (16×2, I2C backpack)

| Wire | ESP32 Pin |
|---|---|
| SDA | GPIO 21 |
| SCL | GPIO 22 |
| VCC | 5V |
| GND | GND |

- I2C address is configured as `0x27` — the other common address for these backpacks is `0x3F`. If the diagnostic sketch reports no I2C ACK, try `0x3F` before assuming a wiring fault.
- If the LCD shows garbled or shifted characters despite a successful I2C ACK, the panel itself (not the wiring or code) is the likely fault.

## Status LEDs + Piezo Buzzer

| Component | ESP32 Pin |
|---|---|
| Red LED | GPIO 17 |
| Yellow LED | GPIO 5 |
| Green LED | GPIO 23 |
| Piezo buzzer | GPIO 16 (PWM, 2kHz) |

Each LED is driven directly from its GPIO through a current-limiting resistor (typically 220Ω–330Ω) to GND — no transistor needed at these current levels.

## Cooling Fan

| Wire | ESP32 Pin |
|---|---|
| PWM control signal | GPIO 32 |

- Driven via `ledcAttach`/`ledcWrite` PWM at 25kHz, 8-bit resolution — this is a **direct PWM signal to a MOSFET or transistor switching the fan**, not through the L298N motor driver pins listed below.
- On boot, the fan briefly kick-starts at a higher duty cycle before settling to a 20% baseline — this is intentional (many small DC fans won't reliably start spinning from a low PWM duty alone).

## Feeding Servo

| Wire | ESP32 Pin |
|---|---|
| Signal | GPIO 33 |
| VCC | 5V (servos draw significant current — do not power from the ESP32's own 3.3V/5V regulator if it's a small hobby servo; use a separate 5V supply sharing common ground) |
| GND | GND |

## ⚠️ Defined But Currently Unused

These pins are still declared in the firmware (`PIN_L298N_IN1` = GPIO 26, `PIN_L298N_IN2` = GPIO 27, `PIN_L298N_ENA` = GPIO 25) but **no code anywhere references them** — they're leftover from an earlier hardware revision that used an L298N motor driver for a component since removed from the system (matching this project's move away from a filtration/pump subsystem earlier in development).

**If you have an L298N board wired to these pins expecting it to do something, it currently won't** — nothing in the firmware drives them. If you don't have anything wired there, no action needed; they're harmless dead definitions.

## Quick Reference — Full Pin Map

| GPIO | Function |
|---|---|
| 4 | DS18B20 data (water + housing temp, shared bus) |
| 5 | Yellow LED |
| 16 | Piezo buzzer (PWM) |
| 17 | Red LED |
| 21 | LCD SDA |
| 22 | LCD SCL |
| 23 | Green LED |
| 25 | *(unused — legacy L298N ENA)* |
| 26 | *(unused — legacy L298N IN1)* |
| 27 | *(unused — legacy L298N IN2)* |
| 32 | Fan PWM control |
| 33 | Feeding servo signal |
| 34 | Turbidity sensor (analog) |
| 35 | pH sensor (analog) |
| 39 | Ammonia sensor (analog) |

## If Multiple Sensors Fail Simultaneously

Per the diagnostic conversation that led to this document: pH, Ammonia, Water Temp, and Housing Temp all failing at once is unusual, since they sit on two electrically independent subsystems (analog ADC vs. OneWire digital). The most likely shared cause is:

1. **Missing or loose shared ground** between the sensor rail and the ESP32
2. **The 4.7kΩ OneWire pull-up resistor missing** (kills both DS18B20 probes at once, but shouldn't affect pH/Ammonia — check this and the ground separately)
3. **Sensor supply rail not actually powered** (check with a multimeter — a rail that reads 0V or an unstable/sagging voltage under load will cause exactly this symptom across every sensor on that rail)

Flash the diagnostic sketch and check the raw voltage/ROM-address output before reflashing the main firmware — it'll tell you definitively which of these is the case.
