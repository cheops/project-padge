# Project Padge

Motion-reactive LED badge powered by an ATtiny85. Sleeps in deep power-down until the LIS3DH accelerometer detects motion, wakes up to play a short LED animation, then goes back to sleep.

## Features

- **Wake on motion** — LIS3DH interrupt triggers wake from ATtiny85 power-down sleep
- **10 WS2812 LEDs** — ripple/rainbow animations on wake
- **Two modes** — short button press cycles between:
  - **Mode 1 (Accel)**: react to accelerometer motion with a ripple burst animation
  - **Mode 2 (Static)**: ignore accelerometer, play a rainbow sweep when woken by button
- **Long press** (~2s) — red flash, then deep sleep
- **Boost converter control** — 5V boost for LEDs is enabled only during animation, disabled during sleep to save power
- **Shift-register button debounce** — glitch-free, non-blocking, ~10µs pin reads keep the boost converter running uninterrupted

## Pin mapping

```
ATtiny85 DIP-8
                ┌──────┐
    (RESET) PB5 ┤1    8├ VCC
(BTN/BOOST) PB3 ┤2    7├ PB2 (SCL)
   (WS2812) PB4 ┤3    6├ PB1 (ACCEL INT)
            GND ┤4    5├ PB0 (SDA)
                └──────┘
```

| Pin | Function | Direction | Notes |
|-----|----------|-----------|-------|
| PB0 | I2C SDA  | Bidir     | USI hardware, to LIS3DH SDA |
| PB1 | LIS3DH INT1 | Input | Active HIGH, latched interrupt from accelerometer |
| PB2 | I2C SCL  | Output    | USI hardware, to LIS3DH SCL |
| PB3 | Button / Boost EN | Shared | Input w/ pullup for button reads; Output HIGH to enable boost converter. Alternates during operation (~10µs input per frame). Add ~100kΩ pull-down on boost EN line. |
| PB4 | WS2812 data | Output | 10 LED chain |
| PB5 | RESET | — | Left as reset for ISP programming |

## Hardware

- **MCU**: ATtiny85 @ 8 MHz internal oscillator
- **Accelerometer**: LIS3DH (I2C, address 0x18 with SA0 to GND)
- **LEDs**: 10× WS2812 / WS2812B
- **Boost converter**: Enabled via PB3, powers 5V LED rail from battery
- **Button**: Momentary, active LOW, connected between PB3 and GND

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
| Motion detected (MODE_ACCEL) | Wake, play ripple animation (~1.5s), sleep |
| Short press | Cycle mode: Accel (green flash) ↔ Static (blue flash), then sleep |
| Long press (~2s) | Force MODE_STATIC (red flash), disable accel interrupt, sleep — only button can wake |
| Short press from MODE_STATIC | Switch back to MODE_ACCEL (green flash), re-enables accel interrupt |

## Power flow

1. **Sleep** — MCU in power-down, boost off (PB3 = input w/ pullup), LIS3DH in low-power 10 Hz mode
2. **Wake** — PCINT fires (motion or button), MCU wakes
3. **Animate** — PB3 set to output HIGH (boost on), LEDs run for ~1.5s
4. **Sleep** — LEDs cleared, PB3 driven LOW then back to input, MCU sleeps

## Circuit notes

- **Decoupling**: 100nF ceramic cap on ATtiny85 VCC, LIS3DH VCC, and each WS2812
- **RESET**: 10kΩ pull-up to VCC on PB5
- **I2C pull-ups**: 4.7kΩ to 3V on SDA and SCL
- **Level shifter**: BSS138 / 2N7002 (Vgs(th) < 2V), gate to 3V, 10kΩ source pull-up, 4.7kΩ drain pull-up to 5V
- **Boost EN pull-down**: 100kΩ to GND on PB3, keeps boost off when pin is input
- **LIS3DH address**: 0x18 (SA0 to GND), or 0x19 (SA0 to VCC)
- **WS2812 data line**: optional 330Ω series resistor close to first LED for EMI protection


### Build size
RAM:   [====      ]  37.7% (used 193 bytes from 512 bytes)
Flash: [========  ]  76.9% (used 6300 bytes from 8192 bytes)