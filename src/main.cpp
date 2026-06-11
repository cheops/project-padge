#include <Arduino.h>
#include <avr/sleep.h>
#include <avr/interrupt.h>
#include <FastLED.h>
#include <TinyWireM.h>

// --- Pin definitions ---
// PB0 = I2C SDA (USI), PB2 = I2C SCL (USI) — fixed by hardware
// NOTE: PB1 is shared between button and LIS3DH INT1 (both active LOW).
//       LIS3DH INT1 is push-pull — a series resistor (~4.7kΩ) is needed
//       between the LIS3DH INT1 output and PB1 to limit contention current
//       when the button pulls LOW while INT1 drives HIGH (inactive).
//       For lower contention current, replace internal pullup with an
//       external 100–220kΩ pullup, increase series resistor to 22–47kΩ,
//       and change INPUT_PULLUP to INPUT below (70–150µA instead of 700µA).
#define BTN_PIN     1   // PB1 - shared: button (active LOW) + accel INT1 (active LOW)
#define BOOST_PIN   3   // PB3 - boost enable (dedicated output)
#define LED_PIN     4   // PB4 - WS2812 data

// --- LED config ---
#define NUM_LEDS    10
#define BRIGHTNESS  255
#define LED_TYPE    WS2812
#define COLOR_ORDER GRB

// --- LIS3DH ---
#define LIS3DH_ADDR       0x18  // SA0 to GND
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

// --- Button timing ---
#define LONG_PRESS_FRAMES  125   // ~2 seconds at 60fps

// --- Button states ---
#define BTN_IDLE      0
#define BTN_SHORT     1
#define BTN_LONG      2

// --- Modes ---
#define MODE_ACCEL  0   // react to accelerometer motion
#define MODE_STATIC 1   // ignore accelerometer, ambient LEDs
#define NUM_MODES   2

CRGBArray<NUM_LEDS> leds;
volatile bool wakeFlag = false;
uint8_t currentMode = MODE_ACCEL;

// --- Shift-register debounce state ---
uint8_t btnShift = 0xFF;    // shift register, 1=released
bool btnDown = false;        // debounced button state
uint8_t btnHoldFrames = 0;   // frames held down
bool btnLongFired = false;   // long press already reported

// --- Animation duration (frames at ~60fps) ---
#define ANIM_FRAMES 90  // ~1.5 seconds of animation per wake

// ============================================================
// I2C helpers for LIS3DH
// ============================================================

void lis_write(uint8_t reg, uint8_t val) {
    TinyWireM.beginTransmission(LIS3DH_ADDR);
    TinyWireM.write(reg);
    TinyWireM.write(val);
    TinyWireM.endTransmission();
}

uint8_t lis_read(uint8_t reg) {
    TinyWireM.beginTransmission(LIS3DH_ADDR);
    TinyWireM.write(reg);
    TinyWireM.endTransmission();
    TinyWireM.requestFrom(LIS3DH_ADDR, (uint8_t)1);
    return TinyWireM.read();
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

bool lis_init() {
    if (lis_read(LIS3DH_WHO_AM_I) != 0x33) return false;

    // 10 Hz, low-power mode, X/Y/Z enabled
    lis_write(LIS3DH_CTRL_REG1, 0x2F);
    // High-pass filter enabled for INT1
    lis_write(LIS3DH_CTRL_REG2, 0x01);
    // Route IA1 interrupt to INT1 pin
    lis_write(LIS3DH_CTRL_REG3, 0x40);
    // ±2g, low-power
    lis_write(LIS3DH_CTRL_REG4, 0x00);
    // Latch INT1
    lis_write(LIS3DH_CTRL_REG5, 0x08);
    // Active LOW — INT1 pin LOW when interrupt active
    lis_write(LIS3DH_CTRL_REG6, 0x02);

    // Threshold ~250mg (16 × 15.625mg at ±2g low-power)
    lis_write(LIS3DH_INT1_THS, 0x10);
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
    wakeFlag = true;
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

void goToSleep() {
    boostOff();

    // Configure accel interrupt based on mode
    if (currentMode == MODE_ACCEL) {
        lis_enableInt();
    } else {
        lis_disableInt();
    }

    // PCINT on PB1 — wakes on button press or accel INT (both active LOW)
    GIMSK |= (1 << PCIE);
    PCMSK = (1 << PCINT1);
    wakeFlag = false;

    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    sleep_enable();
    sei();
    sleep_cpu();
    // --- wakes here ---
    sleep_disable();

    delay(50);
    // Don't clear INT1_SRC here — loop() reads it to determine wake source
    wakeFlag = false;
    btnShift = 0xFF;
    btnDown = false;
    btnHoldFrames = 0;
    btnLongFired = false;
}

// ============================================================
// Button: 0=nothing, 1=short press, 2=long press
// ============================================================

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

// ============================================================
// Play animation then sleep
// ============================================================

void playAndSleep(bool fromAccel) {
    for (uint8_t frame = 0; frame < ANIM_FRAMES; frame++) {
        // Non-blocking button check
        uint8_t btn = btnSample();
        if (btn == BTN_SHORT) {
            currentMode = (currentMode + 1) % NUM_MODES;
            fill_solid(leds, NUM_LEDS,
                       currentMode == MODE_ACCEL ? CRGB::Green : CRGB::Blue);
            FastLED.show();
            delay(200);
            if (fromAccel && currentMode != MODE_ACCEL) {
                break;
            }
            frame = 0;
            continue;
        } else if (btn == BTN_LONG) {
            currentMode = MODE_STATIC;
            fill_solid(leds, NUM_LEDS, CRGB::Red);
            FastLED.show();
            delay(500);
            break;
        }

        if (currentMode == MODE_ACCEL) {
            animAccelReact(frame);
        } else {
            animStatic(frame);
        }
        FastLED.show();
        delay(16);
    }

    goToSleep();
}

// ============================================================
// Setup
// ============================================================

void setup() {
    pinMode(BTN_PIN, INPUT_PULLUP);
    pinMode(BOOST_PIN, OUTPUT);
    digitalWrite(BOOST_PIN, LOW);

    TinyWireM.begin();
    lis_init();

    FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS)
           .setCorrection(TypicalLEDStrip);
    FastLED.setBrightness(BRIGHTNESS);
    FastLED.clear(true);

    // Enable PCINT on shared button/accel pin
    GIMSK |= (1 << PCIE);
    PCMSK = (1 << PCINT1);
    sei();

    currentMode = MODE_ACCEL;

    // Start sleeping immediately — wake on motion or button
    goToSleep();
}

// ============================================================
// Main loop
// ============================================================

void loop() {
    // Determine what woke us by reading INT1_SRC (also clears latch)
    uint8_t intSrc = lis_read(LIS3DH_INT1_SRC);
    bool fromAccel = (intSrc & 0x40);  // IA bit = interrupt was active

    // In MODE_STATIC, ignore accelerometer wakes — go back to sleep
    if (fromAccel && currentMode != MODE_ACCEL) {
        wakeFlag = false;
        goToSleep();
        return;
    }

    // Enable boost for LED output
    boostOn();

    // Disable accel INT for clean button sampling on shared pin
    lis_disableInt();

    // Button wake: check for mode change / long press
    if (!fromAccel) {
        // Run button sampling loop until release is detected
        bool done = false;
        while (!done) {
            uint8_t btn = btnSample();
            if (btn == BTN_SHORT) {
                currentMode = (currentMode + 1) % NUM_MODES;
                fill_solid(leds, NUM_LEDS,
                           currentMode == MODE_ACCEL ? CRGB::Green : CRGB::Blue);
                FastLED.show();
                delay(300);
                done = true;
            } else if (btn == BTN_LONG) {
                currentMode = MODE_STATIC;
                fill_solid(leds, NUM_LEDS, CRGB::Red);
                FastLED.show();
                delay(500);
                done = true;
            }
            delay(16);
        }
        goToSleep();
        return;
    }

    // Accel wake in MODE_ACCEL — play animation then sleep
    playAndSleep(true);
}