#include <Arduino.h>
#include <avr/sleep.h>
#include <avr/interrupt.h>
#include <avr/power.h>
#include <avr/eeprom.h>
#include <FastLED.h>

// --- I2C pin assignment ---
// SoftI2CMaster is a small header-only bit-banger parameterized by pins, so
// both PCB revisions use it — they just differ in which physical pins are
// SDA/SCL. (We deliberately do NOT use TinyWireM/Wire-style libraries here:
// they inherit from Stream/Print, and even --gc-sections can't strip unused
// virtual methods reachable through a vtable, which bloats flash far more
// than a second software I2C pin mapping ever would.)
// Define PCB_SWAPPED_I2C_PINS for PCBs where SCL/SDA are swapped.
// Comment it out for the corrected PCB.
// #define PCB_SWAPPED_I2C_PINS

// uncomment to enable demo mode (cycles through all effects automatically)
// #define DEMO_MODE

#define SDA_PORT PORTB
#define SCL_PORT PORTB

#ifdef PCB_SWAPPED_I2C_PINS
#  define SDA_PIN  2      // PB2 → physical SDA on the swapped PCB
#  define SCL_PIN  0      // PB0 → physical SCL on the swapped PCB
#else
#  define SDA_PIN  0      // PB0 → physical SDA on the corrected PCB
#  define SCL_PIN  2      // PB2 → physical SCL on the corrected PCB
#endif

#include <SoftI2CMaster.h>

// --- Pin definitions ---
// NOTE: PB1 is shared between button and LIS3DH INT1 (both active LOW).
//       LIS3DH INT1 is push-pull — a 10kΩ series resistor is needed
//       between the LIS3DH INT1 output and PB1 to limit contention current
//       when the button pulls LOW while INT1 drives HIGH (inactive).
//       No pullup needed: push-pull INT1 always drives PB1; button to GND
//       wins through the 10kΩ series. Contention current: 3.3V/10k = 330µA.
#define BTN_PIN     1   // PB1 - shared: button (active LOW) + accel INT1 (active LOW)
#define BOOST_PIN   3   // PB3 - boost enable (dedicated output)
#define LED_PIN     4   // PB4 - WS2812 data

// --- LED config ---
#define NUM_LEDS    6
// Powered from 2x CR2032 in parallel (high internal resistance, sags under load).
// Keep peak current tiny: hard-cap total LED draw and run dim. FastLED scales
// brightness down at show() time so estimated draw never exceeds LED_MAX_MA.
#define BRIGHTNESS  255    // global ceiling (~16%), power cap dims further as needed
#define LED_MAX_MA  25    // hard limit on total LED current @5V rail
#define LED_TYPE    WS2812
#define COLOR_ORDER GRB

// --- Face LED mapping (WS2812C-2020-V6 chain, D1->D6 = index 0->5) ---
// D4-D6 are accent dots scattered in the foliage around the fox, not on the face.
#define EYE_L       0   // D1 - left eye
#define EYE_R       1   // D2 - right eye
#define MOUTH       2   // D3 - mouth
#define ACCENT_ML   3   // D4 - foliage accent, middle left
#define ACCENT_TR   4   // D5 - foliage accent, top right
#define ACCENT_BR   5   // D6 - foliage accent, bottom right

// --- LIS3DH ---
#define LIS3DH_ADDR_18    0x18
#define LIS3DH_WHO_AM_I   0x0F
#define LIS3DH_CTRL_REG1  0x20
#define LIS3DH_CTRL_REG2  0x21
#define LIS3DH_CTRL_REG3  0x22
#define LIS3DH_CTRL_REG4  0x23
#define LIS3DH_CTRL_REG5  0x24
#define LIS3DH_CTRL_REG6  0x25
#define LIS3DH_INT1_CFG   0x30
#define LIS3DH_INT1_SRC   0x31
#define LIS3DH_INT1_THS   0x32
#define LIS3DH_INT1_DUR   0x33
#define LIS3DH_OUT_X_L    0x28

// --- Button timing ---
#define LONG_PRESS_FRAMES  125   // ~2 seconds at 60fps

// --- Button states ---
#define BTN_IDLE      0
#define BTN_SHORT     1
#define BTN_LONG      2

// --- Modes ---
#define MODE_ACCEL  0   // react to accelerometer motion
#define MODE_STATIC 1   // ignore accelerometer, react to button presses
#define MODE_FAIL   2   // LIS3DH not detected, show error pattern
#define NUM_MODES   3


CRGBArray<NUM_LEDS> leds;
uint8_t currentMode = MODE_ACCEL;

// --- Shift-register debounce state ---
uint8_t btnShift = 0xFF;    // shift register, 1=released
bool btnDown = false;        // debounced button state
uint8_t btnHoldFrames = 0;   // frames held down
bool btnLongFired = false;   // long press already reported

// --- Animation duration (frames at ~60fps) ---
#define ANIM_FRAMES 90  // ~1.5 seconds of animation per wake

// --- Effect cycling ---
#define NUM_EFFECTS 7
uint8_t currentEffect = 0;

// --- Idle-abort guard ---
// If we wake on the button pin but never see a real press/release resolve,
// give up after this many frames instead of burning the whole ANIM_FRAMES
// budget on a spurious/shared-pin wake.
#define WAKE_CONFIRM_TIMEOUT_FRAMES 40

// Hardcoded LIS address for the current PCB revision.
const uint8_t LIS3DH_ADDR = LIS3DH_ADDR_18;

// --- EEPROM startup hardware-test marker ---
#define HWTEST_MAGIC_OK    0xA5
#define HWTEST_MAGIC_FAIL  0xB4
#define HWTEST_MAGIC_ERASED 0xFF
uint8_t EEMEM eeHwTestMarker;

// ============================================================
// I2C helpers for LIS3DH
// ============================================================

void lis_write_at(uint8_t addr, uint8_t reg, uint8_t val) {
    i2c_start((addr << 1) | 0);
    i2c_write(reg);
    i2c_write(val);
    i2c_stop();
}

uint8_t lis_read_at(uint8_t addr, uint8_t reg) {
    if (!i2c_start((addr << 1) | 0))     { i2c_stop(); return 0; }
    i2c_write(reg);
    if (!i2c_rep_start((addr << 1) | 1)) { i2c_stop(); return 0; }
    uint8_t v = i2c_read(true);  // true = NAK (last byte)
    i2c_stop();
    return v;
}

void lis_readMulti(uint8_t startReg, uint8_t *dst, uint8_t len) {
    if (!i2c_start((LIS3DH_ADDR << 1) | 0))     { i2c_stop(); return; }
    i2c_write(startReg | 0x80);  // auto-increment register address
    if (!i2c_rep_start((LIS3DH_ADDR << 1) | 1)) { i2c_stop(); return; }
    for (uint8_t i = 0; i < len; i++) {
        dst[i] = i2c_read(i == len - 1);  // NAK on last byte, ACK otherwise
    }
    i2c_stop();
}

void lis_write(uint8_t reg, uint8_t val) {
    lis_write_at(LIS3DH_ADDR, reg, val);
}

uint8_t lis_read(uint8_t reg) {
    return lis_read_at(LIS3DH_ADDR, reg);
}

void lis_clearInt() {
    lis_read(LIS3DH_INT1_SRC);  // reading clears the latched interrupt
}

void lis_enableInt() {
    lis_write(LIS3DH_INT1_CFG, 0x2A);  // OR high events on all axes
    lis_clearInt();
}

void lis_disableInt() {
    lis_write(LIS3DH_INT1_CFG, 0x00);  // no interrupt conditions
    lis_clearInt();
}

// Best-effort: push the INT1 pin to a known-safe idle state (nothing
// routed, active-low polarity => idle HIGH) even if WHO_AM_I didn't match.
// A WHO_AM_I mismatch still means the device ACKed and returned a byte, so
// it may well be present and listening — just not configured. Without this,
// a chip left at its power-on-reset default (which can idle INT1 LOW) would
// look electrically identical to a held-down button on the shared PB1 pin:
// no edge ever occurs between "idle" and "pressed", so PCINT never fires
// and the board can never wake up again once in MODE_FAIL.
// This can't help if the chip is truly absent/unpowered/miswired — in that
// case the pin's idle level is whatever the external circuit leaves it at,
// which only a hardware fix (e.g. a pull-up) can address.
void lis_forceSafeIdlePolarity() {
    lis_write(LIS3DH_CTRL_REG3, 0x00);  // route nothing to INT1
    lis_write(LIS3DH_CTRL_REG6, 0x02);  // active-low polarity => idle HIGH
}

bool lis_init() {
    if (lis_read(LIS3DH_WHO_AM_I) != 0x33) return false;

    // 1 Hz, low-power mode, X/Y/Z enabled  (saves ~2µA vs 10 Hz)
    lis_write(LIS3DH_CTRL_REG1, 0x17);
    // High-pass filter enabled for INT1
    lis_write(LIS3DH_CTRL_REG2, 0x01);
    // Route IA1 interrupt to INT1 pin
    lis_write(LIS3DH_CTRL_REG3, 0x40);
    // +/-2g, low-power
    lis_write(LIS3DH_CTRL_REG4, 0x00);
    // Latch INT1
    lis_write(LIS3DH_CTRL_REG5, 0x08);
    // Active LOW (idle HIGH): INT1 pin LOW when interrupt active
    lis_write(LIS3DH_CTRL_REG6, 0x02);

    // Threshold ~125mg (8 x 15.625mg at +/-2g low-power)
    lis_write(LIS3DH_INT1_THS, 0x08);
    // No minimum duration
    lis_write(LIS3DH_INT1_DUR, 0x00);
    // OR combination of high events on all axes
    lis_write(LIS3DH_INT1_CFG, 0x2A);

    lis_clearInt();
    return true;
}

// ============================================================
// Pin Change Interrupt — wakes from power-down
// ============================================================

ISR(PCINT0_vect) {
}

// ============================================================
// Deep sleep
// ============================================================

void boostOn() {
    digitalWrite(BOOST_PIN, HIGH);
    delay(5);  // let boost stabilize
}

void boostOff() {
    FastLED.clear(true);
    digitalWrite(BOOST_PIN, LOW);
}

void flashLedsOneByOne(CRGB color) {
    boostOn();
    FastLED.clear(true);
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        leds[i] = color;
        FastLED.show();
        delay(500);
    }
    FastLED.clear();
    FastLED.show();
    boostOff();
}

void signalModeAccel() {
    boostOn();
    CRGB color = CRGB::Green;

    // Step 1: Surround foliage lights up
    FastLED.clear();
    leds[ACCENT_ML] = color;
    leds[ACCENT_TR] = color;
    leds[ACCENT_BR] = color;
    FastLED.show();
    delay(80);

    // Step 2: Mouth lights up
    leds[MOUTH] = color;
    FastLED.show();
    delay(80);

    // Step 3: Eyes open (Eyes lit up)
    leds[EYE_L] = color;
    leds[EYE_R] = color;
    FastLED.show();
    delay(80);

    // Turn off
    FastLED.clear();
    FastLED.show();
    boostOff();
}


void signalModeStatic() {
    boostOn();
    CRGB color = CRGB::Red;

    // Step 1: Everything turns on first briefly
    fill_solid(leds, NUM_LEDS, color);
    FastLED.show();
    delay(80);

    // Step 2: Eyes shut (off)
    leds[EYE_L] = CRGB::Black;
    leds[EYE_R] = CRGB::Black;
    FastLED.show();
    delay(80);

    // Step 3: Mouth shuts (off)
    leds[MOUTH] = CRGB::Black;
    FastLED.show();
    delay(80);

    // Step 4: Surrounds shut down completely right before sleep
    FastLED.clear();
    FastLED.show();
    boostOff();
}


void goToSleep() {
    boostOff();

    // Configure accel interrupt based on mode
    if (currentMode == MODE_ACCEL) {
        lis_enableInt();
    } else {
        lis_disableInt();
    }

    // --- Power-down pin preparation ---
    // Release I2C bus: disable USI, SDA/SCL as inputs (external pull-ups hold HIGH)
    USICR = 0;
    DDRB &= ~((1 << DDB0) | (1 << DDB2));
    PORTB &= ~((1 << PB0) | (1 << PB2));
    // LED pin: drive LOW as an output (not floating input). Boost is off so
    // WS2812 VDD is already 0V; holding the data line at a firm 0V matches
    // that and avoids both backfeeding the LEDs and CMOS input-buffer
    // leakage from an undefined floating level.
    DDRB |= (1 << DDB4);
    PORTB &= ~(1 << PB4);

    // Shared button/INT1 pin (PB1): in MODE_FAIL, fall back to the internal
    // pull-up so a genuinely-absent/unresponsive LIS3DH can't leave the pin
    // permanently LOW (which would look identical to a held button and mask
    // every future wake). In ACCEL/STATIC modes the LIS3DH's own push-pull
    // INT1 output already defines the idle level, so the pull-up would only
    // add needless contention current there — leave it disabled.
    // NOTE: if the LIS3DH is present but failed only its ID check (see
    // lis_forceSafeIdlePolarity) and its INT1 output is stuck actively
    // driving LOW, this pull-up will fight it and draw a few tens of µA
    // continuously through the 10k series resistor, even during sleep.
    // That's an accepted tradeoff for MODE_FAIL specifically: it's already
    // a degraded/error state, and a wakeable badge beats a battery-efficient
    // brick.
    if (currentMode == MODE_FAIL) {
        PORTB |= (1 << PB1);
    } else {
        PORTB &= ~(1 << PB1);
    }

    // PCINT on PB1 — wakes on button press or accel INT (both active LOW)
    GIMSK |= (1 << PCIE);
    PCMSK = (1 << PCINT1);

    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    sleep_enable();
    sei();
    sleep_cpu();
    // --- wakes here ---
    sleep_disable();

    // Re-enable I2C (loop() reads INT1_SRC immediately)
    i2c_init();
    // Re-configure LED pin as output
    pinMode(LED_PIN, OUTPUT);

    delay(50);
    // Don't clear INT1_SRC here — loop() reads it to determine wake source
    btnShift = 0xFF;
    btnDown = false;
    btnHoldFrames = 0;
    btnLongFired = false;
}

// ============================================================
// Non-blocking button with shift-register debounce
// Call once per frame. Returns BTN_IDLE / BTN_SHORT / BTN_LONG.
// Short press fires on release. Long press fires while held.
// ============================================================

uint8_t btnSample() {
    uint8_t bit = digitalRead(BTN_PIN) ? 1 : 0;

    // Shift in the new sample (0=pressed, 1=released)
    btnShift = (btnShift << 1) | bit;

    // Debounced transitions: 8 consistent samples
    if (btnShift == 0x00 && !btnDown) {
        // Button just pressed (8 consecutive LOW)
        btnDown = true;
        btnHoldFrames = 0;
        btnLongFired = false;
    } else if (btnShift == 0xFF && btnDown) {
        // Button just released (8 consecutive HIGH)
        btnDown = false;
        if (!btnLongFired) {
            return BTN_SHORT;
        }
    }

    // Count held frames for long press
    if (btnDown) {
        btnHoldFrames++;
        if (btnHoldFrames >= LONG_PRESS_FRAMES && !btnLongFired) {
            btnLongFired = true;
            return BTN_LONG;
        }
    }

    return BTN_IDLE;
}

// ============================================================
// LED animations
// ============================================================

void animAccelReact(uint8_t frame) {
    // Ripple burst from center, fading out toward end
    uint8_t center = NUM_LEDS / 2;
    uint8_t fade = (frame < ANIM_FRAMES - 20) ? 255
                   : map(frame, ANIM_FRAMES - 20, ANIM_FRAMES, 255, 0);
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        uint8_t dist = (i > center) ? (i - center) : (center - i);
        uint8_t bri = qadd8(255 - dist * 50, 0);
        leds[i] = CHSV(frame * 15 + dist * 25, 255,
                        scale8(scale8(bri, cubicwave8(frame * 8 + dist * 30)), fade));
    }
}

void animStatic(uint8_t frame) {
    // Rainbow sweep that fades out at the end
    uint8_t fade = (frame < ANIM_FRAMES - 20) ? 100
                   : map(frame, ANIM_FRAMES - 20, ANIM_FRAMES, 100, 0);
    fill_rainbow(leds, NUM_LEDS, frame * 3, 25);
    nscale8(leds, NUM_LEDS, fade);
}

void animSparkle(uint8_t frame) {
    // Random sparkles that twinkle and fade
    uint8_t fade = (frame < ANIM_FRAMES - 20) ? 255
                   : map(frame, ANIM_FRAMES - 20, ANIM_FRAMES, 255, 0);
    // Decay existing pixels
    nscale8(leds, NUM_LEDS, 180);
    // Randomly ignite new sparkles
    if (random8() < 100) {
        uint8_t pos = random8(NUM_LEDS);
        leds[pos] = CHSV(random8(), 160, scale8(255, fade));
    }
}

void animHeartbeat(uint8_t frame) {
    // Classic lub-dub heartbeat — two quick red pulses then pause
    // Beat cycle: ~60 frames. Two pulses in first 30, rest is dark.
    uint8_t phase = frame % 60;
    uint8_t bri = 0;
    if (phase < 8)        bri = ease8InOutQuad(phase * 32);       // lub up
    else if (phase < 14)  bri = ease8InOutQuad((14 - phase) * 42); // lub down
    else if (phase < 20)  bri = ease8InOutQuad((phase - 14) * 42); // dub up
    else if (phase < 28)  bri = ease8InOutQuad((28 - phase) * 32); // dub down
    // Fade out at end of animation
    uint8_t fade = (frame < ANIM_FRAMES - 20) ? 255
                   : map(frame, ANIM_FRAMES - 20, ANIM_FRAMES, 255, 0);
    bri = scale8(bri, fade);
    fill_solid(leds, NUM_LEDS, CHSV(0, 255, bri));  // red
}

void animEyeBlink(uint8_t frame) {
    // Eyes glow steadily, blink shut twice, then fade
    FastLED.clear();
    uint8_t fade = (frame < ANIM_FRAMES - 20) ? 255
                   : map(frame, ANIM_FRAMES - 20, ANIM_FRAMES, 255, 0);
    // Blink pattern: open, shut at frame 25 and 50 for ~5 frames each
    bool shut = (frame >= 25 && frame < 30) || (frame >= 50 && frame < 55);
    uint8_t eyeBri = shut ? 0 : fade;
    leds[EYE_L] = CHSV(130, 200, eyeBri);  // cool blue-white eyes
    leds[EYE_R] = CHSV(130, 200, eyeBri);
}

void animSmile(uint8_t frame) {
    // Eyes, mouth and foliage accents breathe warmly.
    // Breathing (not steady-on) keeps average current low for CR2032.
    FastLED.clear();
    uint8_t fade = (frame < ANIM_FRAMES - 20) ? 255
                   : map(frame, ANIM_FRAMES - 20, ANIM_FRAMES, 255, 0);
    // Slow breathing envelope: pulses 0..255 so LEDs never hold full-on
    uint8_t breath = scale8(cubicwave8(frame * 4), fade);
    // Eyes: warm white, breathing
    leds[EYE_L] = CHSV(32, 150, scale8(180, breath));
    leds[EYE_R] = CHSV(32, 150, scale8(180, breath));
    // Mouth: warm orange, eases on
    if (frame > 10) leds[MOUTH] = CHSV(20, 255, scale8(160, breath));
    // Foliage accents: green glow, staggered on
    if (frame > 18) leds[ACCENT_ML] = CHSV(96, 220, scale8(120, breath));
    if (frame > 22) leds[ACCENT_TR] = CHSV(96, 220, scale8(120, breath));
    if (frame > 26) leds[ACCENT_BR] = CHSV(96, 220, scale8(120, breath));
}

void animWink(uint8_t frame) {
    // One eye winks while the other stays lit, mouth grins
    FastLED.clear();
    uint8_t fade = (frame < ANIM_FRAMES - 20) ? 255
                   : map(frame, ANIM_FRAMES - 20, ANIM_FRAMES, 255, 0);
    // Right eye: always on
    leds[EYE_R] = CHSV(45, 200, scale8(200, fade));
    // Left eye: wink shut between frames 20-50
    bool wink = (frame >= 20 && frame < 50);
    leds[EYE_L] = CHSV(45, 200, wink ? 0 : scale8(200, fade));
    // Mouth: cheerful grin
    leds[MOUTH] = CHSV(25, 255, scale8(140, fade));
    // Foliage accents: green glow
    uint8_t leaf = scale8(110, fade);
    leds[ACCENT_ML] = CHSV(96, 220, leaf);
    leds[ACCENT_TR] = CHSV(96, 220, leaf);
    leds[ACCENT_BR] = CHSV(96, 220, leaf);
}

// Dispatches to whichever effect is currently selected. Single source of
// truth for "what does frame N look like" — used by the one animation loop.
void renderEffect(uint8_t frame) {
    switch (currentEffect) {
        case 0: animAccelReact(frame); break;
        case 1: animStatic(frame);     break;
        case 2: animSparkle(frame);    break;
        case 3: animHeartbeat(frame);  break;
        case 4: animEyeBlink(frame);   break;
        case 5: animSmile(frame);      break;
        case 6: animWink(frame);       break;
    }
}

// Blocks until the button has been physically released (a few consecutive
// HIGH reads, to shrug off contact bounce right at release). Used after a
// long-press mode toggle so we never re-enter sleep while the button is
// still down — otherwise a continued hold risks another PCINT wake, another
// debounce-and-hold cycle, and a second unwanted toggle before the user
// lets go.
void waitForButtonRelease() {
    uint8_t highStreak = 0;
    while (highStreak < 8) {
        if (digitalRead(BTN_PIN)) {
            highStreak++;
        } else {
            highStreak = 0;
        }
        delay(10);
    }
}

// Toggles between ACCEL and STATIC mode and plays the matching feedback
// animation. Only reachable via a long press.
void toggleMode() {
    if (currentMode == MODE_ACCEL) {
        currentMode = MODE_STATIC;
        signalModeStatic();
    } else if (currentMode == MODE_STATIC) {
        currentMode = MODE_ACCEL;
        signalModeAccel();
    }
}

// ============================================================
// Unified wake handler: button sampling + animation playback live
// in this one per-frame loop, instead of being split between a
// blocking "wait for the button" loop and a separate animation loop
// that re-implemented its own button handling.
//
// fromAccel == true:  accel-triggered wake. Button sampling is skipped
//   entirely (PB1 is shared with LIS3DH INT1, so sampling it here would
//   just pick up interrupt-pin noise) and the current effect plays once,
//   starting cleanly at frame 0.
//
// fromAccel == false: button-triggered wake.
//   - Every frame we sample the button.
//   - Nothing is rendered until the press that woke us resolves into a
//     short or long event — this is what keeps every animation
//     consistent: it always starts at frame 0 of the effect that was
//     just selected, never a stray frame of whatever effect played
//     last wake.
//   - A short press (first one, or any later one during playback)
//     advances to the next effect and (re)starts the animation at
//     frame 0.
//   - A long press toggles ACCEL <-> STATIC mode, plays the matching
//     mode-change signal animation, and goes straight to sleep — no
//     effect animation plays for a long press.
//   - Until that first short/long event resolves, we also guard
//     against spurious/shared-pin wakes: if the button never actually
//     goes down (or a press never resolves) within
//     WAKE_CONFIRM_TIMEOUT_FRAMES, we bail out and go back to sleep
//     instead of burning the full animation budget for nothing.
// ============================================================

void handleWakeAndSleep(bool fromAccel) {
    uint8_t frame = 0;
    uint8_t idleFrames = 0;
    bool resolved = fromAccel;  // accel wakes start rendering immediately

    if (fromAccel) {
        // Accel wake in MODE_ACCEL — cycle to the next effect before playing it.
        currentEffect = (currentEffect + 1) % NUM_EFFECTS;
        boostOn();  // rendering starts on frame 0 of this same call
    }

    while (true) {
        if (!fromAccel) {
            uint8_t btn = btnSample();

            if (btn == BTN_SHORT) {
                currentEffect = (currentEffect + 1) % NUM_EFFECTS;
                frame = 0;
                if (!resolved) {
                    boostOn();  // first confirmed press this wake — power up the LEDs
                }
                resolved = true;
            } else if (btn == BTN_LONG) {
                toggleMode();
                waitForButtonRelease();
                goToSleep();
                return;
            } else if (!resolved) {
                // Still waiting to see whether this wake corresponds to a
                // genuine button interaction; nothing renders yet.
                if (!btnDown) {
                    idleFrames++;
                    if (idleFrames >= WAKE_CONFIRM_TIMEOUT_FRAMES) {
                        goToSleep();
                        return;
                    }
                } else {
                    idleFrames = 0;
                }
            }
        }

        if (resolved) {
            renderEffect(frame);
            FastLED.show();
        }

        delay(16);

        if (resolved) {
            frame++;
            if (frame >= ANIM_FRAMES) {
                FastLED.clear();
                FastLED.show();
                break;
            }
        }
    }

    #ifdef DEMO_MODE
    // In demo mode, we don't go to sleep after each effect
    // the loop() will just advance to the next one automatically.
    boostOff();
    
    #else
    
    // In normal operation, we go back to sleep after each effect finishes.
    goToSleep();
    
    #endif
}

// ============================================================
// Setup
// ============================================================

void setup() {
    // Force CPU clock prescaler to /1.
    // If CKDIV8 fuse is set, this removes the runtime divide-by-8 so
    // FastLED/WS2812 timing matches the configured F_CPU.
    clock_prescale_set(clock_div_1);

    // --- Disable unused peripherals for power savings ---
    ADCSRA &= ~(1 << ADEN);  // disable ADC (~260µA saved)
    PRR |= (1 << PRADC) | (1 << PRTIM1);  // shut down ADC clock + Timer1
    ACSR |= (1 << ACD);      // disable analog comparator (~25µA saved)

    pinMode(BTN_PIN, INPUT);  // no pullup; INT1 push-pull drives PB1
    pinMode(BOOST_PIN, OUTPUT);
    digitalWrite(BOOST_PIN, LOW);

    #ifndef DEMO_MODE
    // Bring up I2C for LIS communication.
    i2c_init();
    #endif

    FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS)
           .setCorrection(TypicalLEDStrip);
    FastLED.setBrightness(BRIGHTNESS);
    
    #ifndef DEMO_MODE
    // Hard current ceiling: auto-dims each show() so CR2032 never browns out
    FastLED.setMaxPowerInVoltsAndMilliamps(5, LED_MAX_MA);
    #endif

    FastLED.clear(true);

    #ifndef DEMO_MODE
    bool lisOk = false;
    // First-boot hardware test (persisted in EEPROM).
    uint8_t marker = eeprom_read_byte(&eeHwTestMarker);
    if (marker == HWTEST_MAGIC_OK) {
        lisOk = true;
        lis_init();
    } else if (marker == HWTEST_MAGIC_FAIL) {
        // LIS3DH not detected on previous boot. We never talk to it on this
        // path otherwise, but still make a best-effort attempt to force its
        // INT1 pin to a safe idle state (see lis_forceSafeIdlePolarity) —
        // harmless if the chip is truly absent, but can save us from a
        // permanently-unwakeable board if it's present and just failed the
        // WHO_AM_I check.
        lisOk = false;
        lis_forceSafeIdlePolarity();
    } else {
        // HWTEST_MAGIC_ERASED: first boot after programming — run LIS3DH detection test
        // or any other bytes found in EEPROM are treated as erased (0xFF).
        lisOk = lis_init();
        if (!lisOk) {
            lis_forceSafeIdlePolarity();
        }
        eeprom_write_byte(&eeHwTestMarker, lisOk ? HWTEST_MAGIC_OK : HWTEST_MAGIC_FAIL);
    } 

    if (lisOk) {
        // LIS3DH detected — show success pattern and mark OK in EEPROM
        currentMode = MODE_ACCEL;
        flashLedsOneByOne(CRGB::Green);
    } else {
        // LIS3DH not detected — show error pattern and mark FAIL in EEPROM
        currentMode = MODE_FAIL;
        flashLedsOneByOne(CRGB::Blue);
    }

    // Enable PCINT on shared button/accel pin
    GIMSK |= (1 << PCIE);
    PCMSK = (1 << PCINT1);
    // Clear any stale pin-change flag once after reset/programming.
    GIFR |= (1 << PCIF);
    sei();

    // Start sleeping immediately.
    goToSleep();
    #endif
}

// ============================================================
// Main loop
// ============================================================

void loop() {
    if (currentMode == MODE_FAIL) {
        // LIS3DH not detected — no point querying it, just show the error
        // pattern and go back to sleep.
        boostOn();
        flashLedsOneByOne(CRGB::Blue);
        boostOff();
        goToSleep();
        return;
    }

    #ifdef DEMO_MODE

    // Demo mode: cycle through all effects automatically
    handleWakeAndSleep(true);  // true = fromAccel, so it plays immediately
    delay(1000);  // brief pause between effects
    
    #else
    
    // Determine what woke us by reading INT1_SRC (also clears latch)
    uint8_t intSrc = lis_read(LIS3DH_INT1_SRC);
    bool fromAccel = (intSrc & 0x40);  // IA bit = interrupt was active

    // In MODE_STATIC, ignore accelerometer wakes — go back to sleep
    if (fromAccel && currentMode != MODE_ACCEL) {
        goToSleep();
        return;
    }

    // Disable accel INT for clean button sampling on shared pin
    lis_disableInt();

    handleWakeAndSleep(fromAccel);
    
    #endif

}
