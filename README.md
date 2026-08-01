# Project Padge

Motion-reactive LED badge powered by an ATtiny85. Sleeps in deep power-down until the LIS3DH accelerometer detects motion, wakes up to play a short LED animation, then goes back to sleep.

## Features

- **Wake on motion** — LIS3DH interrupt triggers wake from ATtiny85 power-down sleep
- **6 WS2812C LEDs** — 7 cycling effects, one per wake-up
- **Face layout** — 2 eyes, 1 mouth, 3 foliage accent dots (WS2812C-2020-V6, D1→D6 = index 0→5)
- **Two modes**, toggled only by a long press:
  - **Mode 1 (Accel)**: react to accelerometer motion *or* button press
  - **Mode 2 (Static)**: ignore accelerometer, wake only by button
- **Short press** — cycles to the next LED effect and plays it (works in either mode)
- **Long press** (~2s) — toggles mode: green flash into Accel, red flash into Static
- **Accelerometer self-test** — result cached in EEPROM at boot; green flash on success, blue error pattern on failure (and again on every subsequent wake while it stays failed)
- **Watchdog timer** — self-resets within ~2s if the active (awake) code ever hangs; fully disabled during power-down sleep so it can't cut a long sleep short
- **Boost converter control** — 5V boost for LEDs is enabled only once an animation actually starts rendering, disabled again before sleep (PB3 dedicated output)
- **Shift-register button debounce** — glitch-free, non-blocking reads on shared PB1 pin
- **Software I2C** — bit-banged (no hardware USI/Wire-style library), pins configurable in firmware for either PCB revision

## LED Effects

Each wake-up cycles to the next effect:

| # | Effect | LEDs lit | Description |
|---|--------|----------|-------------|
| 0 | Ripple burst | All 6 | Colorful ripple from center outward |
| 1 | Rainbow sweep | All 6 | Slow rainbow that fades out |
| 2 | Sparkle | ~1–2 | Random twinkles that decay |
| 3 | Heartbeat | All 6 | Red lub-dub double-pulse |
| 4 | Eye blink | 2 (eyes) | Blue-white eyes blink shut twice |
| 5 | Smile | 6 (eyes + mouth + accents) | Warm eyes, mouth on, foliage accents glow |
| 6 | Wink | 6 (eyes + mouth + accents) | One eye winks, mouth grins |

Effects 2 and 4 are low-power (few LEDs lit at a time).

## LED layout

WS2812C-2020-V6 chain, data runs D1→D6 (FastLED index 0→5). D1–D3 are on the
fox's face; D4–D6 are accent dots scattered in the foliage around the artwork.

| Index | Designator | Position |
|-------|-----------|----------|
| 0 | D1 | Left eye |
| 1 | D2 | Right eye |
| 2 | D3 | Mouth |
| 3 | D4 | Foliage accent, middle left |
| 4 | D5 | Foliage accent, top right |
| 5 | D6 | Foliage accent, bottom right |

## Pin mapping

```
ATtiny85 DIP-8
                ┌──────┐
    (RESET) PB5 ┤1    8├ VCC
 (BOOST EN) PB3 ┤2    7├ PB2
   (WS2812) PB4 ┤3    6├ PB1 (BTN + ACCEL INT)
            GND ┤4    5├ PB0
                └──────┘
```

I2C is bit-banged (`SoftI2CMaster`) on PB0/PB2 for both PCB revisions — there's
no separate hardware-USI code path. Which pin is SDA vs. SCL is a firmware
choice, not a library choice: toggle `PCB_SWAPPED_I2C_PINS` in `src/main.cpp`
to match your board.

| Pin | Function | Direction | Notes |
|-----|----------|-----------|-------|
| PB0 | I2C | Bidir | SDA on the swapped PCB (`PCB_SWAPPED_I2C_PINS` defined, current default), SCL on the corrected PCB |
| PB1 | Button + LIS3DH INT1 | Input | Shared pin, both active LOW. No pullup in normal operation (INT1 push-pull drives line); internal pull-up enabled as a fallback only in MODE_FAIL. 10kΩ series resistor between LIS3DH INT1 and PB1. |
| PB2 | I2C | Bidir | SCL on the swapped PCB (current default), SDA on the corrected PCB |
| PB3 | Boost EN | Output | Dedicated output. HIGH = boost on, LOW = boost off. Add ~100kΩ pull-down to keep boost off during reset. |
| PB4 | WS2812 data | Output | 6 LED chain. Held LOW as an output (not floating) during sleep. |
| PB5 | RESET | — | Left as reset for ISP programming |

## Hardware

- **MCU**: ATtiny85 @ 8 MHz internal oscillator
- **Accelerometer**: LIS3DH (I2C, address 0x18 with SA0 to GND)
- **LEDs**: 6× WS2812C-2020-V6
- **Boost converter**: Enabled via PB3 (dedicated output), powers 5V LED rail from battery
- **Battery**: 2× CR2032 in parallel (~3V). High internal resistance, so LEDs are driven conservatively (see Power budget)
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
| Motion detected (MODE_ACCEL only) | Wake, play next effect (~1.5s), sleep |
| Short press (either mode) | Wake, cycle to next effect, play it (~1.5s), sleep |
| Long press (~2s), currently Accel | Toggle to Static (red flash), disable accel interrupt, sleep — only button wakes it from here |
| Long press (~2s), currently Static | Toggle to Accel (green flash), re-enable accel interrupt, sleep |

A long press only ever fires one toggle — the firmware waits for the button to
be physically released before sleeping, so holding it down longer than 2
seconds can't flip the mode back and forth.

## Power budget

Running from 2× CR2032 in parallel, which sag badly under load, so the LEDs are driven as gently as possible:

- **Global brightness** capped low (`BRIGHTNESS` ≈ 16%).
- **Hard current limit** — `FastLED.setMaxPowerInVoltsAndMilliamps(5, LED_MAX_MA)` auto-dims every frame so total LED draw never exceeds `LED_MAX_MA` (25 mA), preventing brown-out.
- **Blink/breathe over steady-on** — effects pulse or fade rather than holding LEDs at full brightness (e.g. the smile breathes instead of staying lit).
- **Short bursts** — LEDs only run for the ~1.5 s animation per wake, then everything sleeps.

Adjust `BRIGHTNESS` and `LED_MAX_MA` in [src/main.cpp](src/main.cpp) to trade brightness for battery life.

## Power flow

1. **Sleep** — MCU in power-down, boost off (PB3 LOW), LIS3DH in low-power 1 Hz mode, accel INT enabled if in MODE_ACCEL. ADC, analog comparator, and USI are disabled; I2C pins are released as inputs and the WS2812 data pin is held LOW as an output, to prevent current leaks and floating-input draw.
2. **Wake** — PCINT on PB1 fires (motion or button), MCU wakes, I2C bus re-initialized
3. **Identify source** — read LIS3DH `INT1_SRC` register: IA bit set = motion, clear = button press
4. **Confirm & animate** — for a button wake, nothing is rendered (and the boost stays off) until the press resolves into a short or long event, so an animation never starts with a stray frame of whatever effect played last time. Once resolved: boost turns on, accel INT is disabled for clean button sampling, and LEDs run for ~1.5s (short press) or the mode-toggle flash (long press).
5. **Sleep** — LEDs cleared, PB3 LOW, re-enable accel INT if in MODE_ACCEL, peripherals shut down, MCU sleeps

## Watchdog

The ATtiny85's watchdog timer (`WDTO_2S`) is armed for the entire active
(awake) portion of the code — boot, animations, button handling, I2C calls —
and explicitly disabled around `SLEEP_MODE_PWR_DOWN`, then re-armed
immediately on wake. This split matters: power-down sleep can legitimately
last hours between motion events, far longer than any sane watchdog timeout,
so the WDT must not tick during sleep at all.

If any part of the active code ever hangs for more than ~2s without reaching
a `wdt_reset()` — an I2C call stuck waiting on a bus condition that never
resolves, for example — the chip resets itself and resumes normal operation,
rather than staying unresponsive indefinitely. This is generic protection
against unexpected hangs; it isn't targeted at any one confirmed cause.

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

## Known limitations

- **If the LIS3DH fails its startup ID check**, the firmware makes a best-effort attempt to force its INT1 pin to a safe idle-HIGH state (in case it's electrically present and just returned an unexpected ID), and additionally enables the ATtiny85's internal pull-up on the shared PB1 pin while in MODE_FAIL. This guarantees a genuinely-absent/unwired accelerometer can't leave the button permanently unable to wake the board. The tradeoff: if the chip is present, failed only its ID check, and its INT1 output ends up stuck actively driving LOW (e.g. after a brownout resets it back to power-on defaults), the internal pull-up will contend with it through the 10kΩ series resistor and draw a continuous few tens of µA — including through sleep — until the board is reflashed or repaired. This only applies while in MODE_FAIL, which is already meant to be a "something's wrong" state.

### Build size

Flash usage depends on which I2C pin backend and library options are active — run `pio run` after any change and check the summary rather than relying on numbers here, since they'll drift as the firmware evolves.