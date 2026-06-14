#include <Arduino.h>
#include <avr/sleep.h>
#include <avr/interrupt.h>
#include <FastLED.h>
#include <TinyWireM.h>

// --- Pin definitions ---
// PB0 = I2C SDA (USI), PB2 = I2C SCL (USI) — fixed by hardware
// NOTE: PB1 is shared between button and LIS3DH INT1 (both active LOW).
//       LIS3DH INT1 is push-pull — a 22kΩ series resistor is needed
//       between the LIS3DH INT1 output and PB1 to limit contention current
//       when the button pulls LOW while INT1 drives HIGH (inactive).
//       External 100kΩ pullup on PB1 (do NOT use internal pullup).
//       Contention current: ~150µA. Divider: 3.3V × 22k/(100k+22k) = 0.60V < VIL.
#define BTN_PIN     1   // PB1 - shared: button (active LOW) + accel INT1 (active LOW)
#define BOOST_PIN   3   // PB3 - boost enable (dedicated output)
#define LED_PIN     4   // PB4 - WS2812 data

// --- LED config ---
#define NUM_LEDS    7
#define BRIGHTNESS  255
#define LED_TYPE    WS2812
#define COLOR_ORDER GRB

// --- Face LED mapping (adjust when PCB layout is known) ---
#define EYE_L       2
#define EYE_R       4
#define MOUTH_0     5
#define MOUTH_1     6
#define MOUTH_2     0

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

// --- Effect cycling ---
#define NUM_EFFECTS 7
uint8_t currentEffect = 0;

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

    // 1 Hz, low-power mode, X/Y/Z enabled  (saves ~2µA vs 10 Hz)
    lis_write(LIS3DH_CTRL_REG1, 0x17);
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

    // --- Power-down pin preparation ---
    // Release I2C bus: disable USI, SDA/SCL as inputs (external pull-ups hold HIGH)
    USICR = 0;
    DDRB &= ~((1 << DDB0) | (1 << DDB2));
    PORTB &= ~((1 << PB0) | (1 << PB2));
    // LED pin: input, no pull-up (boost off, avoid leaking into WS2812)
    DDRB &= ~(1 << DDB4);
    PORTB &= ~(1 << PB4);

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

    // Re-enable I2C (loop() reads INT1_SRC immediately)
    TinyWireM.begin();
    // Re-configure LED pin as output
    pinMode(LED_PIN, OUTPUT);

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
    // Eyes and mouth light up warmly — mouth sweeps on
    FastLED.clear();
    uint8_t fade = (frame < ANIM_FRAMES - 20) ? 255
                   : map(frame, ANIM_FRAMES - 20, ANIM_FRAMES, 255, 0);
    // Eyes: steady warm white
    leds[EYE_L] = CHSV(32, 150, scale8(180, fade));
    leds[EYE_R] = CHSV(32, 150, scale8(180, fade));
    // Mouth: sweep on left-to-right, warm orange
    uint8_t mouthBri = scale8(140, fade);
    if (frame > 10) leds[MOUTH_0] = CHSV(20, 255, mouthBri);
    if (frame > 18) leds[MOUTH_1] = CHSV(20, 255, mouthBri);
    if (frame > 26) leds[MOUTH_2] = CHSV(20, 255, mouthBri);
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
    uint8_t mBri = scale8(120, fade);
    leds[MOUTH_0] = CHSV(25, 255, mBri);
    leds[MOUTH_1] = CHSV(25, 255, mBri);
    leds[MOUTH_2] = CHSV(25, 255, mBri);
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

        switch (currentEffect) {
            case 0: animAccelReact(frame); break;
            case 1: animStatic(frame);     break;
            case 2: animSparkle(frame);    break;
            case 3: animHeartbeat(frame);  break;
            case 4: animEyeBlink(frame);   break;
            case 5: animSmile(frame);      break;
            case 6: animWink(frame);       break;
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
    // --- Disable unused peripherals for power savings ---
    ADCSRA &= ~(1 << ADEN);  // disable ADC (~260µA saved)
    PRR |= (1 << PRADC) | (1 << PRTIM1);  // shut down ADC clock + Timer1
    ACSR |= (1 << ACD);      // disable analog comparator (~25µA saved)

    pinMode(BTN_PIN, INPUT);  // external 100kΩ pullup on PB1
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

    // Accel wake in MODE_ACCEL — cycle effect and play animation then sleep
    currentEffect = (currentEffect + 1) % NUM_EFFECTS;
    playAndSleep(true);
}