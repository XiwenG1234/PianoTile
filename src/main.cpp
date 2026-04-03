#include <Adafruit_NeoPixel.h>

// ─── Configuration ──────────────────────────────────────────────────────────

#define NUM_KEYS          8
#define LEDS_PER_KEY      4          // 4 LEDs sit under each key
#define LED_PIN           17
#define LED_COUNT         (NUM_KEYS * LEDS_PER_KEY)  // 32 total
#define LED_BRIGHTNESS    80         // 0-255 — keep low to protect USB power

#define TOUCH_THRESHOLD   40         // readings BELOW this = touched
#define DEBOUNCE_MS       200

#define CALIBRATION_PRINT_MS 0       // set to e.g. 500 to see raw values

// ─── Per-key colours (R, G, B) ──────────────────────────────────────────────
// One vivid colour per key so you can instantly see which is active.

const uint8_t KEY_COLORS[NUM_KEYS][3] = {
    {255,   0,   0},   // Key 1 — Red
    {255, 100,   0},   // Key 2 — Orange
    {200, 200,   0},   // Key 3 — Yellow
    {  0, 255,   0},   // Key 4 — Green
    {  0, 200, 200},   // Key 5 — Cyan
    {  0,   0, 255},   // Key 6 — Blue
    {150,   0, 255},   // Key 7 — Purple
    {255,   0, 150},   // Key 8 — Pink
};

// ─── Pin / Key Definitions ───────────────────────────────────────────────────

struct TouchKey {
    uint8_t     gpio;
    uint8_t     touchNum;
    const char* label;
};

const TouchKey KEYS[NUM_KEYS] = {
    {  4,  0, "Key 1 (GPIO4 / T0)" },
    {  2,  2, "Key 2 (GPIO2 / T2)" },
    { 15,  3, "Key 3 (GPIO15 / T3)" },
    { 13,  4, "Key 4 (GPIO13 / T4)" },
    { 12,  5, "Key 5 (GPIO12 / T5)" },
    { 14,  6, "Key 6 (GPIO14 / T6)" },
    { 27,  7, "Key 7 (GPIO27 / T7)" },
    { 33,  8, "Key 8 (GPIO33 / T8)" },
};

// ─── Globals ─────────────────────────────────────────────────────────────────

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

bool     keyDown[NUM_KEYS]       = {false};
uint32_t lastReleaseMs[NUM_KEYS] = {0};

// ─── LED Helpers ─────────────────────────────────────────────────────────────

// Light up all 4 LEDs for key index i in its colour.
void keyLEDOn(uint8_t i) {
    int base = i * LEDS_PER_KEY;
    uint32_t col = strip.Color(KEY_COLORS[i][0],
                               KEY_COLORS[i][1],
                               KEY_COLORS[i][2]);
    for (int l = 0; l < LEDS_PER_KEY; l++) {
        strip.setPixelColor(base + l, col);
    }
    strip.show();
}

// Turn off all 4 LEDs for key index i.
void keyLEDOff(uint8_t i) {
    int base = i * LEDS_PER_KEY;
    for (int l = 0; l < LEDS_PER_KEY; l++) {
        strip.setPixelColor(base + l, 0);
    }
    strip.show();
}

// ─── Setup ───────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);

    // Boot the LED strip first — all off
    strip.begin();
    strip.setBrightness(LED_BRIGHTNESS);
    strip.clear();
    strip.show();

    // Brief startup animation: sweep each key colour across its LEDs
    for (uint8_t i = 0; i < NUM_KEYS; i++) {
        keyLEDOn(i);
        delay(60);
        keyLEDOff(i);
    }

    delay(1500);   // let Serial monitor connect

    Serial.println("===========================================");
    Serial.println(" Piano Tiles — Touch + LED Test");
    Serial.println("===========================================");
    Serial.printf(" Threshold  : %d  (lower = touched)\n", TOUCH_THRESHOLD);
    Serial.printf(" Debounce   : %d ms\n", DEBOUNCE_MS);
    Serial.printf(" LEDs/key   : %d  (%d total on GPIO%d)\n",
                  LEDS_PER_KEY, LED_COUNT, LED_PIN);
    Serial.println("-------------------------------------------");
    Serial.println(" Touch a key → its LEDs light up.");
    Serial.println(" Release    → LEDs turn off.");
    Serial.println("===========================================\n");
}

// ─── Main Loop ───────────────────────────────────────────────────────────────

void loop() {
    uint32_t now = millis();

    for (uint8_t i = 0; i < NUM_KEYS; i++) {
        uint16_t raw     = touchRead(KEYS[i].gpio);
        bool     touched = (raw < TOUCH_THRESHOLD);

        // Rising edge — finger placed
        if (touched && !keyDown[i]) {
            if ((now - lastReleaseMs[i]) >= DEBOUNCE_MS) {
                keyDown[i] = true;
                keyLEDOn(i);
                Serial.printf({"Try to light up key %d\n"}, i + 1);
                Serial.printf("  ▶  TOUCHED  %s  (raw=%d)\n",
                              KEYS[i].label, raw);
            }
        }

        // Falling edge — finger lifted
        if (!touched && keyDown[i]) {
            keyDown[i]       = false;
            lastReleaseMs[i] = now;
            keyLEDOff(i);
            Serial.printf("     released %s\n", KEYS[i].label);
        }
    }

#if CALIBRATION_PRINT_MS > 0
    static uint32_t lastPrint = 0;
    if (now - lastPrint >= CALIBRATION_PRINT_MS) {
        lastPrint = now;
        Serial.print("[RAW]");
        for (uint8_t i = 0; i < NUM_KEYS; i++) {
            Serial.printf("  K%d=%3d", i + 1, touchRead(KEYS[i].gpio));
        }
        Serial.println();
    }
#endif

    delay(10);
}
