# CRX-Pulse Hardware Wiring Guide

Firmware version referenced: `12.0.0` (`crx_pulse.ino`). The bench sketch `crx_pulse_test.ino` uses the **same board and the same pins** — anything below applies to both.

All pin numbers here are exactly what the firmware `#define`s. If you rewire anything, change the matching `#define` too.

> **Note:** an earlier revision of this document described a different board (two DS18B20 probes on a shared GPIO 4 bus, status LEDs, a piezo buzzer, a MOSFET fan on GPIO 32, and "unused" L298N pins). None of that matches this firmware. Wire to the table below, not to that older layout.

## Power Requirements

| Rail | Used by |
|---|---|
| 5V | LCD backlight, MQ-137 heater, feeder servo, both L298N logic rails |
| 7–12V | L298N motor supply (cooling fan + DC rotator) |
| 3.3V | ESP32 logic, DS18B20, analog sensor reference |
| GND | Common ground — **every component must share ground with the ESP32**, including anything on 5V or the motor supply |

A shared, solid ground across every component is the single most common cause of "sensor not connected" symptoms when the wiring otherwise looks right. Check it first if several sensors fail at once.

The MG946R-class servo and the MQ-137 heater both draw real current. Do not run them from the ESP32 board's own regulator — give them a separate 5V supply that shares ground.

## Full Pin Map

| GPIO | Function | Notes |
|---|---|---|
| 2 | WiFi status LED | HIGH = connected |
| 16 | DHT11 data | Housing/ambient temperature + humidity |
| 18 | DS18B20 data | Water temperature |
| 19 | Feeder servo signal | 50 Hz, 500–2400 µs pulse range |
| 21 | LCD SDA | I2C |
| 22 | LCD SCL | I2C |
| 23 | L298N #2 IN2 | Cooling fan direction |
| 25 | L298N #1 ENA | DC rotator PWM |
| 26 | L298N #1 IN1 | Rotator direction |
| 27 | L298N #1 IN2 | Rotator direction |
| 32 | pH sensor (analog) | ADC1 |
| 33 | L298N #2 ENA | Cooling fan PWM |
| 34 | Turbidity sensor (analog) | ADC1, input-only |
| 35 | MQ-137 ammonia (analog, via divider) | ADC1, input-only |
| 17 | L298N #2 IN1 | Cooling fan direction |

Every analog sensor is on **ADC1**, which is the part that keeps working while WiFi is active. ADC2 pins cannot be read at all with WiFi up — that is why none are used here.

## DHT11 — Housing Temperature + Humidity

| Wire | ESP32 Pin |
|---|---|
| Data | GPIO 16 |
| VCC | 3.3V (or 5V — both work) |
| GND | GND |

- A 10kΩ pull-up between data and VCC is recommended; most breakout modules already have one fitted.
- The DHT11 **drops individual reads routinely** — this is normal for the part. The firmware keeps the last good value for `SENSOR_GRACE_MS` (15 s) before declaring the sensor disconnected, so a single dropped read neither blanks the app nor cuts the cooling fan.
- This sensor is what drives the cooling fan. If it is genuinely unplugged, the fan is held OFF rather than guessing a temperature.

## DS18B20 — Water Temperature

| Wire | ESP32 Pin |
|---|---|
| Data | GPIO 18 |
| VCC | 3.3V |
| GND | GND |

- **A 4.7kΩ pull-up between data and 3.3V is required.** Without it the bus can look like it works intermittently or report zero devices — the most common DS18B20 wiring mistake.
- Only **one** probe is on this bus in this build; the firmware reads index 0.
- Resolution is set to 11 bits (0.125 °C steps, ~375 ms conversion). The value is displayed to 0.1 °C, so nothing useful is lost and the sensor pass stays quick.

## Turbidity Sensor (SEN0189)

| Wire | ESP32 Pin |
|---|---|
| Signal (analog out) | GPIO 34 |
| VCC | 5V |
| GND | GND |

- **No voltage divider** on this input — the board's output already sits inside the ADC range at the calibrated operating point.
- Calibration (in the firmware): `V_CLEAR = 1.3646 V` → 0 % relative turbidity, `V_HIGH = 1.1916 V` → 100 %. Note the inversion: **more turbid means a LOWER voltage.** Conversion is piecewise linear through those ends and the three band edges below.
- Status bands, as voltages and as the percentages the app receives:

  | Status | Voltage | Percent |
  |---|---|---|
  | Clear | ≥ 1.33015 V | 0 – 19.9 % |
  | Slightly Turbid | ≥ 1.28065 V | 19.9 – 48.5 % |
  | Turbid | ≥ 1.22865 V | 48.5 – 78.6 % |
  | Highly Turbid | < 1.22865 V | 78.6 – 100 % |

## pH Sensor

| Wire | ESP32 Pin |
|---|---|
| Signal (analog out) | GPIO 32 |
| VCC | 5V (most analog pH boards regulate down internally) |
| GND | GND |

- The signal passes through a **2:1 divider** (`PH_DIVIDER_RATIO = 2.0`), so the firmware doubles the measured ADC voltage to reconstruct the board's output.
- Expected ADC voltage for a connected, powered probe: roughly **0.05 V – 3.25 V**. Stuck at 0.00 V or pinned at 3.30 V is a power/signal wiring fault, not a calibration issue — the firmware reports exactly that range as `ph_valid`.
- Calibration is **3-point** and lives in the firmware as the voltages measured in each buffer:

  | Buffer | Constant | Current value |
  |---|---|---|
  | 6.86 | `voltage_pH7` | 1.950 V |
  | 4.01 | `voltage_pH4` | 2.447 V |
  | 9.18 | `voltage_pH9` | 1.425 V |

  Recalibrate by reading `pH Voltage` from the serial monitor in each buffer and replacing these three numbers.

- Readings are median-sampled (15 ADC reads) and then low-pass filtered with a 0.02 pH deadband, so a stable buffer does not make the last digit flicker. Allow a few seconds for the displayed value to settle after moving probes between buffers.

## Ammonia Sensor (MQ-137)

| Wire | ESP32 Pin |
|---|---|
| Signal (AOUT) via divider | GPIO 35 |
| VCC (heater) | 5V |
| GND | GND |

Wire the divider exactly like this — the MQ-137 can swing above 3.3 V and would otherwise damage the input:

```
MQ-137 AO ──[10k]──┬──[10k]── GND
                   │
                 GPIO 35
```

GPIO 35 therefore sees **half** the MQ-137 output; the firmware multiplies by `AMMONIA_DIVIDER_RATIO = 2.0` to recover it.

- **MQ-series sensors need a genuine warm-up.** Give the heater ~60 s from power-on before trusting a reading. This is the part, not a fault.
- The heater is the heaviest continuous load on the 5V rail. If 5V is shared and marginal, check it with a multimeter under load.
- The mg/L estimate is interpolated between three Sera reference points measured on the bench:

  | Sensor voltage | Concentration |
  |---|---|
  | 0.2655 V | 0.00 mg/L |
  | 0.4030 V | 0.17 mg/L |
  | 0.4400 V | 0.53 mg/L |

  Outside that band the firmware **clamps to the nearest calibrated end** and marks the value `[est]` on the serial monitor. It deliberately does not extrapolate a concentration it has never been calibrated against. Treat this as a relative indicator ("is ammonia rising?"), not a lab figure.

## LCD Display (16×2, I2C backpack)

| Wire | ESP32 Pin |
|---|---|
| SDA | GPIO 21 |
| SCL | GPIO 22 |
| VCC | 5V |
| GND | GND |

- Address is `0x27` in the firmware. The other common address for these backpacks is `0x3F` — try that before assuming a wiring fault if there is no I2C ACK.
- Garbled or shifted characters despite a good ACK points at the panel itself, not the wiring or code.
- The display cycles five pages every 2.5 s: water temp + pH, housing temp + humidity, turbidity, ammonia, then both motors.

## Feeder Servo

| Wire | ESP32 Pin |
|---|---|
| Signal | GPIO 19 |
| VCC | 5V (separate supply, shared ground) |
| GND | GND |

- `180° = CLOSED / resting`, `160° = OPEN / feeding`. The first pulse the servo ever receives commands 180°, so it settles closed instead of sweeping from 0 at power-on.
- The firmware reserves LEDC timer 0 for the servo **before** setting up the two `analogWrite()` motors, so the motors cannot take the timer the servo needs. Keep that ordering if you edit `setup()`.
- Dose is set by how long the door stays open:

  | Grams | Open time |
  |---|---|
  | 5 | 400 ms |
  | 10 | 800 ms |
  | 15 | 1200 ms |
  | 20 | 1600 ms |
  | 25 | 2000 ms |
  | 30 | 2400 ms |

## L298N #1 — DC Rotator

| L298N Pin | ESP32 Pin |
|---|---|
| ENA | GPIO 25 |
| IN1 | GPIO 26 |
| IN2 | GPIO 27 |
| VCC (motor supply) | 7–12V |
| GND | **shared with ESP32 GND** |

Direction is fixed in `setup()` (IN1 LOW, IN2 HIGH) and speed comes from PWM on ENA. The rotator runs **before a feed**: it spins for `FeedController::ROTATOR_RUN_MS` (12 s) at the speed selected in the app, then stops. The firmware waits `FeedController::SETTLE_MS` (1.5 s) before the servo opens and drops the feed. Rotator speed Off skips the spin and the settle; the servo still dispenses.

If the rotator spins the wrong way, swap IN1/IN2 — either the wires or the two `digitalWrite()` calls in `setup()`.

## L298N #2 — Cooling Fan

| L298N Pin | ESP32 Pin |
|---|---|
| ENA | GPIO 33 |
| IN1 | GPIO 17 |
| IN2 | GPIO 23 |
| VCC (motor supply) | 7–12V |
| GND | **shared with ESP32 GND** |

Fully automatic, driven by the DHT11 housing temperature:

| Housing temp | Fan PWM |
|---|---|
| below 25 °C | 0 (off) |
| 25 → 40 °C | ramps 191 → 255 |
| above 40 °C | 255 (full) |

191 is the lowest duty that reliably starts this fan under load, which is why the ramp begins there rather than at 0. The mobile app shows the same curve — `computeFanSpeed()` in `lib/core/fan_speed_util.dart` mirrors these four numbers, so change them in both places.

## WiFi Status LED

| Wire | ESP32 Pin |
|---|---|
| LED anode (through 220–330Ω) | GPIO 2 |
| LED cathode | GND |

Solid on = associated with the router. Off = disconnected; the firmware retries every 10 s.

## WiFi Credentials

The SSID and password are **not** in `crx_pulse.ino`. The sketch reads them from `secrets.h`, which is gitignored:

```cpp
#include "secrets.h"

const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;
```

Two ways to set them:

- **Desktop app** (`desktop/`) — type the network, press Compile & Upload. It writes `secrets.h` and flashes the board. See `desktop/README.md`.
- **By hand** — `cp secrets.h.example secrets.h` and fill it in.

The sketch will not compile without one of these; a missing `secrets.h` is a "No such file or directory" error, not a silent fallback.

**2.4 GHz only.** The ESP32 cannot join a 5 GHz network, and that failure is indistinguishable from a wrong password — check the band first when it will not connect.

## Serial Bench Keys (115200 baud)

The real control path is the mobile app via Supabase. These exist for bench testing:

| Key | Action |
|---|---|
| `7` `8` `9` `a` `b` `c` | Feed 5 / 10 / 15 / 20 / 25 / 30 g (full cycle, including the rotator) |
| `2` `3` `4` `5` `6` | Rotator motor OFF / 25% / 50% / 75% / 100% |

The cooling fan has no manual key — it is temperature-driven only.

## If Multiple Sensors Fail Simultaneously

pH, ammonia, turbidity and water temperature sit on two electrically independent subsystems (analog ADC vs OneWire digital), so all of them failing together points at something shared:

1. **Missing or loose shared ground** between the sensor rail and the ESP32.
2. **Sensor supply rail not actually powered** — check with a multimeter *under load*; a rail that sags when the servo moves or the MQ-137 heater warms up causes exactly this.
3. **The 4.7kΩ OneWire pull-up missing** — this kills the DS18B20 only, so if pH and ammonia are also dead, look at 1 and 2 first.

The serial monitor prints raw ADC counts and voltages alongside every reading. That tells you immediately whether a sensor is pinned at a rail (wiring) or reading a plausible voltage that converts badly (calibration).

## Offline Control — the Device Access Point

The board stays in `WIFI_AP_STA`: it keeps retrying the router **and** raises its own `CRX-Pulse-XXXXXX` network. Joining that network with the app gives full control even when neither the tank nor the phone has internet.

| Route | Method | Body | Purpose |
|---|---|---|---|
| `/api/live` | GET | — | Current reading (existing route) |
| `/api/status` | GET | — | Device id, firmware, uptime, AP clients (existing route) |
| `/api/schedules` | GET | — | Cached feeds, each with its index |
| `/api/time` | POST | `device_id`, `epoch_seconds` | Seed the board clock from the phone |
| `/api/feed` | POST | `device_id`, `grams`, `rotator_pwm` | Feed now |
| `/api/command` | POST | `device_id`, `type`, `value?` | Servo/motor test, log stream |
| `/api/schedules` | POST | `device_id`, `time`, `feed_date?`, `grams`, `rotator_pwm`, `active`, `index?` | Add a feed, or overwrite the one at `index` |
| `/api/schedules/delete` | POST | `device_id`, `index` | Remove a feed by index |

Every POST must carry the board's actual `DEVICE_ID`; a mismatched id is rejected with 400. Accepted writes reply with `{"ok":true,"device_id":...}` so the app can tell the board from a stranger that happens to answer on `192.168.4.1`.

`time` must be zero-padded `HH:MM` and `feed_date` zero-padded `YYYY-MM-DD`; malformed values are rejected with 400 rather than stored as a feed that can never fire. `/api/schedules/delete` requires an explicit `index` — omitting it is rejected, never taken as slot 0.

### What keeps working with no router

- Sensors, LCD and the temperature-driven fan run exactly as online.
- Scheduled feeding fires from the board's own cache — the last set of schedules Supabase sent is written to `Preferences` and restored on boot, so a power cut with no WiFi does not lose it.
- The app reads live values, feeds manually, drives the test actuators and edits schedules over the AP.
- The app seeds the UTC clock once on first contact (`/api/time`). The firmware still applies its UTC+8 offset, so dated feeds work offline.

**Limitation:** the ESP32 has no battery-backed clock. If it powers up with no router *and* the app never joins its AP to seed the time, the board has no trustworthy date and dated feeds cannot fire — every-day schedules still do. Once seeded, the clock runs until the next power cut.

Firmware build tag for the current build (offline control plus the turbidity/pH calibration update): `2026.11.24-sensor-calibration`.
