# Project Padge

Motion-reactive LED badge powered by an ATtiny85. Sleeps in deep power-down until the LIS3DH accelerometer detects motion, wakes up to play a short LED animation, then goes back to sleep.

## Features

- **Wake on motion** — LIS3DH interrupt triggers wake from ATtiny85 power-down sleep
- **7 WS2812 LEDs** — 7 cycling effects, one per wake-up
- **Face layout** — 2 LEDs for eyes, 3 for mouth (positions configurable in code)
- **Two modes** — short button press cycles between:
  - **Mode 1 (Accel)**: react to accelerometer motion
  - **Mode 2 (Static)**: ignore accelerometer, wake only by button
- **Long press** (~2s) — red flash, then deep sleep
- **Boost converter control** — 5V boost for LEDs is enabled only during animation (PB3 dedicated output), disabled during sleep to save power
- **Shift-register button debounce** — glitch-free, non-blocking reads on shared PB1 pin

## LED Effects

Each wake-up cycles to the next effect:

| # | Effect | LEDs lit | Description |
|---|--------|----------|-------------|
| 0 | Ripple burst | All 7 | Colorful ripple from center outward |
| 1 | Rainbow sweep | All 7 | Slow rainbow that fades out |
| 2 | Sparkle | ~1–2 | Random twinkles that decay |
| 3 | Heartbeat | All 7 | Red lub-dub double-pulse |
| 4 | Eye blink | 2 (eyes) | Blue-white eyes blink shut twice |
| 5 | Smile | 5 (eyes + mouth) | Warm eyes, mouth sweeps on |
| 6 | Wink | 5 (eyes + mouth) | One eye winks, mouth grins |

Effects 2 and 4–6 are low-power (few LEDs lit at a time).

## Pin mapping

```
ATtiny85 DIP-8
                ┌──────┐
    (RESET) PB5 ┤1    8├ VCC
 (BOOST EN) PB3 ┤2    7├ PB2 (SCL)
   (WS2812) PB4 ┤3    6├ PB1 (BTN + ACCEL INT)
            GND ┤4    5├ PB0 (SDA)
                └──────┘
```

| Pin | Function | Direction | Notes |
|-----|----------|-----------|-------|
| PB0 | I2C SDA  | Bidir     | USI hardware, to LIS3DH SDA |
| PB1 | Button + LIS3DH INT1 | Input | Shared pin, both active LOW. No pullup needed (INT1 push-pull drives line). 10kΩ series resistor between LIS3DH INT1 and PB1. |
| PB2 | I2C SCL  | Output    | USI hardware, to LIS3DH SCL |
| PB3 | Boost EN | Output    | Dedicated output. HIGH = boost on, LOW = boost off. Add ~100kΩ pull-down to keep boost off during reset. |
| PB4 | WS2812 data | Output | 7 LED chain |
| PB5 | RESET | — | Left as reset for ISP programming |

## Hardware

- **MCU**: ATtiny85 @ 8 MHz internal oscillator
- **Accelerometer**: LIS3DH (I2C, address 0x18 with SA0 to GND)
- **LEDs**: 7× WS2812 / WS2812B
- **Boost converter**: Enabled via PB3 (dedicated output), powers 5V LED rail from battery
- **Button**: Momentary, active LOW, connected between PB1 and GND (shared with LIS3DH INT1)

## Build

Requires [PlatformIO](https://platformio.org/).

```bash
pio run              # compile
pio run -t upload    # flash via Arduino-as-ISP (stk500v1)
```

Upload is configured for `/dev/ttyUSB0` at 19200 baud. Adjust `upload_port` in `platformio.ini` as needed.

## Button

| Action | Effect |
|--------|--------|
| Motion detected (MODE_ACCEL) | Wake, play next effect (~1.5s), sleep |
| Short press | Cycle mode: Accel (green flash) ↔ Static (blue flash), then sleep |
| Long press (~2s) | Force MODE_STATIC (red flash), disable accel interrupt, sleep — only button can wake |
| Short press from MODE_STATIC | Switch back to MODE_ACCEL (green flash), re-enables accel interrupt |

## Power flow

1. **Sleep** — MCU in power-down, boost off (PB3 LOW), LIS3DH in low-power 1 Hz mode, accel INT enabled if in MODE_ACCEL. ADC, analog comparator, and USI are disabled; I2C and LED pins are released as inputs to prevent current leaks through pull-ups.
2. **Wake** — PCINT on PB1 fires (motion or button), MCU wakes, I2C bus re-initialized
3. **Identify source** — read LIS3DH `INT1_SRC` register: IA bit set = motion, clear = button press
4. **Animate** — PB3 HIGH (boost on), accel INT disabled for clean button sampling, LEDs run for ~1.5s
5. **Sleep** — LEDs cleared, PB3 LOW, re-enable accel INT if in MODE_ACCEL, peripherals shut down, MCU sleeps

## Circuit notes

- **Decoupling**: 100nF ceramic cap on ATtiny85 VCC, LIS3DH VCC, and each WS2812
- **RESET**: 10kΩ pull-up to VCC on PB5
- **I2C pull-ups**: 10kΩ to 3V on SDA and SCL (sufficient for 100kHz with short traces and two devices)
- **Level shifter**: BSS138 / 2N7002 (Vgs(th) < 2V), gate to 3V, 10kΩ source pull-up, 4.7kΩ drain pull-up to 5V
- **Boost EN pull-down**: 100kΩ to GND on PB3, keeps boost off during reset/startup
- **LIS3DH INT1 series resistor**: 10kΩ between LIS3DH INT1 output and PB1. LIS3DH INT1 is push-pull active LOW, so PB1 is always actively driven — no pullup needed. When the button pulls PB1 LOW while INT1 is driving HIGH (idle, no motion), the resistor limits contention current to 3.3V/10k = 330µA, only while held.
- **PB1 pullup**: none. INT1 push-pull drives both states; button to GND wins through the 10kΩ series (drops INT1-high to ~0V). Internal pullup disabled.
- **LIS3DH address**: 0x18 (SA0 to GND), or 0x19 (SA0 to VCC)
- **WS2812 data line**: optional 330Ω series resistor close to first LED for EMI protection


### Build size
```
RAM:   [====      ]  36.5% (used 187 bytes from 512 bytes)
Flash: [========= ]  87.4% (used 7160 bytes from 8192 bytes)
```