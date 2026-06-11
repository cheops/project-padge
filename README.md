# Project Padge

Motion-reactive LED badge powered by an ATtiny85. Sleeps in deep power-down until the LIS3DH accelerometer detects motion, wakes up to play a short LED animation, then goes back to sleep.

## Features

- **Wake on motion** — LIS3DH interrupt triggers wake from ATtiny85 power-down sleep
- **10 WS2812 LEDs** — ripple/rainbow animations on wake
- **Two modes** — short button press cycles between:
  - **Mode 1 (Accel)**: react to accelerometer motion with a ripple burst animation
  - **Mode 2 (Static)**: ignore accelerometer, play a rainbow sweep when woken by button
- **Long press** (~2s) — red flash, then deep sleep
- **Boost converter control** — 5V boost for LEDs is enabled only during animation (PB3 dedicated output), disabled during sleep to save power
- **Shift-register button debounce** — glitch-free, non-blocking reads on shared PB1 pin

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
| PB1 | Button + LIS3DH INT1 | Input | Shared pin, both active LOW. Internal pullup. Series resistor (~4.7kΩ) between LIS3DH INT1 and PB1 to limit contention current. |
| PB2 | I2C SCL  | Output    | USI hardware, to LIS3DH SCL |
| PB3 | Boost EN | Output    | Dedicated output. HIGH = boost on, LOW = boost off. Add ~100kΩ pull-down to keep boost off during reset. |
| PB4 | WS2812 data | Output | 10 LED chain |
| PB5 | RESET | — | Left as reset for ISP programming |

## Hardware

- **MCU**: ATtiny85 @ 8 MHz internal oscillator
- **Accelerometer**: LIS3DH (I2C, address 0x18 with SA0 to GND)
- **LEDs**: 10× WS2812 / WS2812B
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
| Motion detected (MODE_ACCEL) | Wake, play ripple animation (~1.5s), sleep |
| Short press | Cycle mode: Accel (green flash) ↔ Static (blue flash), then sleep |
| Long press (~2s) | Force MODE_STATIC (red flash), disable accel interrupt, sleep — only button can wake |
| Short press from MODE_STATIC | Switch back to MODE_ACCEL (green flash), re-enables accel interrupt |

## Power flow

1. **Sleep** — MCU in power-down, boost off (PB3 LOW), LIS3DH in low-power 10 Hz mode, accel INT enabled if in MODE_ACCEL
2. **Wake** — PCINT on PB1 fires (motion or button), MCU wakes
3. **Identify source** — read LIS3DH `INT1_SRC` register: IA bit set = motion, clear = button press
4. **Animate** — PB3 HIGH (boost on), accel INT disabled for clean button sampling, LEDs run for ~1.5s
5. **Sleep** — LEDs cleared, PB3 LOW, re-enable accel INT if in MODE_ACCEL, MCU sleeps

## Circuit notes

- **Decoupling**: 100nF ceramic cap on ATtiny85 VCC, LIS3DH VCC, and each WS2812
- **RESET**: 10kΩ pull-up to VCC on PB5
- **I2C pull-ups**: 4.7kΩ to 3V on SDA and SCL
- **Level shifter**: BSS138 / 2N7002 (Vgs(th) < 2V), gate to 3V, 10kΩ source pull-up, 4.7kΩ drain pull-up to 5V
- **Boost EN pull-down**: 100kΩ to GND on PB3, keeps boost off during reset/startup
- **LIS3DH INT1 series resistor**: 4.7kΩ between LIS3DH INT1 output and PB1. LIS3DH INT1 is push-pull active LOW; when button pulls PB1 LOW while INT1 is driving HIGH (idle), the resistor limits contention current to ~0.7mA. For lower contention, disable internal pullup (use `INPUT` in code), add an external 100–220kΩ pullup on PB1, and increase series resistor to 22–47kΩ (reduces contention to 70–150µA).
- **LIS3DH address**: 0x18 (SA0 to GND), or 0x19 (SA0 to VCC)
- **WS2812 data line**: optional 330Ω series resistor close to first LED for EMI protection


### Build size
```
RAM:   [====      ]  37.7% (used 193 bytes from 512 bytes)
Flash: [=======   ]  74.4% (used 6094 bytes from 8192 bytes)
```