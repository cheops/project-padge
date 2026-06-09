#include <Arduino.h>
#include <avr/sleep.h>
#include <avr/interrupt.h>
#include <FastLED.h>
#include <TinyWireM.h>

// --- Pin definitions ---
// PB0 = I2C SDA (USI), PB2 = I2C SCL (USI) — fixed by hardware
#define BTN_BOOST_PIN 3 // PB3 - shared: button (input, active LOW) / boost enable (output HIGH)
#define LED_PIN     4   // PB4 - WS2812 data
#define ACCEL_INT   1   // PB1 - LIS3DH INT1

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
    pinMode(BTN_BOOST_PIN, OUTPUT);
    digitalWrite(BTN_BOOST_PIN, HIGH);
    delay(5);  // let boost stabilize
}

void boostOff() {
    FastLED.clear(true);
    digitalWrite(BTN_BOOST_PIN, LOW);
    // Switch back to input for button / PCINT wake
    pinMode(BTN_BOOST_PIN, INPUT_PULLUP);
}

void goToSleep() {
    boostOff();

    // Configure accel interrupt based on mode
    if (currentMode == MODE_ACCEL) {
        lis_enableInt();
    } else {
        lis_disableInt();
    }

    // Only enable PCINT for accel pin in MODE_ACCEL
    GIMSK |= (1 << PCIE);
    PCMSK = (1 << PCINT3);  // button always
    if (currentMode == MODE_ACCEL) {
        PCMSK |= (1 << PCINT1);  // accel INT
    }
    wakeFlag = false;

    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    sleep_enable();
    sei();
    sleep_cpu();
    // --- wakes here ---
    sleep_disable();

    delay(50);
    lis_clearInt();
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
    // Brief switch to input to read the pin (~10µs)
    pinMode(BTN_BOOST_PIN, INPUT_PULLUP);
    delayMicroseconds(10);
    uint8_t bit = digitalRead(BTN_BOOST_PIN) ? 1 : 0;
    pinMode(BTN_BOOST_PIN, OUTPUT);
    digitalWrite(BTN_BOOST_PIN, HIGH);

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

        // Drain any accel interrupts that fired during animation
        if (wakeFlag) {
            lis_clearInt();
            wakeFlag = false;
            if (currentMode == MODE_ACCEL && fromAccel) {
                frame = 0;  // re-trigger: restart animation
            }
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
    pinMode(BTN_BOOST_PIN, INPUT_PULLUP);
    pinMode(ACCEL_INT, INPUT);

    TinyWireM.begin();
    lis_init();

    FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS)
           .setCorrection(TypicalLEDStrip);
    FastLED.setBrightness(BRIGHTNESS);
    FastLED.clear(true);

    // Enable PCINT on button and accel interrupt pins
    GIMSK |= (1 << PCIE);
    PCMSK = (1 << PCINT3) | (1 << PCINT1);  // both enabled at startup
    sei();

    currentMode = MODE_ACCEL;

    // Start sleeping immediately — wake on motion or button
    goToSleep();
}

// ============================================================
// Main loop
// ============================================================

void loop() {
    // Determine what woke us
    bool fromAccel = (digitalRead(ACCEL_INT) == HIGH);

    // In MODE_STATIC, ignore accelerometer wakes — go back to sleep
    // (shouldn't happen since accel INT is disabled, but safety check)
    if (fromAccel && currentMode != MODE_ACCEL) {
        lis_clearInt();
        wakeFlag = false;
        goToSleep();
        return;
    }

    // Enable boost for LED output
    boostOn();

    // Button wake with no accel: check for mode change / long press
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