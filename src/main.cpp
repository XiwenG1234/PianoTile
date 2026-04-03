#include <cstdint>
#include <Arduino.h>   

/*
#include <Arduino.h>    

#define S0  32
#define S1  33
#define S2  34
#define S3  35
#define EN  12
#define SIG 17

int rowPins[4] = {13, 14, 27, 26};
volatile int activeRow[8] = {0, 2, 1, 0, 3, 1, 2, 0};

void selectColumn(uint8_t col) {
    digitalWrite(S0, (col >> 0) & 1);
    digitalWrite(S1, (col >> 1) & 1);
    digitalWrite(S2, (col >> 2) & 1);
    digitalWrite(S3, (col >> 3) & 1);
}

void IRAM_ATTR tdmRefresh() {
    static uint8_t currentRow = 0;
    for (int r = 0; r < 4; r++) digitalWrite(rowPins[r], LOW);
    digitalWrite(EN, HIGH);

    for (uint8_t col = 0; col < 8; col++) {
        if (activeRow[col] == currentRow) {
            selectColumn(col);
            digitalWrite(SIG, LOW);
            digitalWrite(EN, LOW);
            delayMicroseconds(5);
            digitalWrite(EN, HIGH);
        }
    }

    digitalWrite(rowPins[currentRow], HIGH);
    currentRow = (currentRow + 1) % 4;
}

void setup() {
    Serial.begin(115200);

    int pins[] = {S0, S1, S2, S3, EN, SIG};
    for (int p : pins) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }
    for (int r = 0; r < 4; r++) { pinMode(rowPins[r], OUTPUT); digitalWrite(rowPins[r], LOW); }

    hw_timer_t *timer = timerBegin(0, 80, true);
    timerAttachInterrupt(timer, &tdmRefresh, true);
    timerAlarmWrite(timer, 500, true);
    timerAlarmEnable(timer);
}

void loop() {
    static uint32_t last = 0;
    if (millis() - last > 500) {
        // Move tile down — update activeRow here
        activeRow[0] = (activeRow[0] + 1) % 4;
        last = millis();
    }
}
*/

/*
 * Piano Tiles — Capacitive Touch Key Test
 * ESP32-DEVKITC-32UE
 *
 * Pins:  GPIO4  → Key 1 (T0)
 *        GPIO2  → Key 2 (T2)
 *        GPIO15 → Key 3 (T3)
 *        GPIO13 → Key 4 (T4)
 *        GPIO12 → Key 5 (T5)
 *        GPIO14 → Key 6 (T6)
 *        GPIO27 → Key 7 (T7)
 *        GPIO33 → Key 8 (T8)
 *
 * Open Serial Monitor at 115200 baud.
 * Touch a key → its label prints once per touch event.
 *
 * CALIBRATION:
 *   1. Run with nothing touching the keys.
 *   2. Observe the raw values printed for each pin.
 *   3. Set TOUCH_THRESHOLD below that idle value (lower = touched on ESP32).
 *      Typical idle: 60-80. Typical touched: 5-20.
 *      Start with 40 and tune from there.
 */

// ─── Configuration ──────────────────────────────────────────────────────────

// Number of touch pins
#define NUM_KEYS 8

// Threshold: readings BELOW this are treated as a touch.
// Lower the value → less sensitive (harder to trigger).
// Raise the value → more sensitive (easier to trigger, more false positives).
#define TOUCH_THRESHOLD 40

// How long (ms) a key must stay untouched before it can fire again.
// Prevents a single press from printing dozens of lines.
#define DEBOUNCE_MS 200

// Print raw readings every N ms (set 0 to disable — use for calibration only).
#define CALIBRATION_PRINT_MS 0   // e.g. change to 500 to see raw values

// ─── Pin / Key Definitions ───────────────────────────────────────────────────

struct TouchKey {
    uint8_t     gpio;      // GPIO number
    uint8_t     touchNum;  // Touch channel (Tx)
    const char* label;     // Human-readable name
};

// Order matches physical key layout left → right
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

// ─── State ───────────────────────────────────────────────────────────────────

bool     keyDown[NUM_KEYS]       = {false};
uint32_t lastReleaseMs[NUM_KEYS] = {0};

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Read the touch channel that belongs to a given key index.
uint16_t readKey(uint8_t idx) {
    return touchRead(KEYS[idx].gpio);
}

void setup() {
    Serial.begin(115200);
    delay(2000);   // let USB-CDC settle

    Serial.println("===========================================");
    Serial.println(" Piano Tiles — Touch Key Test");
    Serial.println("===========================================");
    Serial.printf(" Threshold : %d (lower = touched)\n", TOUCH_THRESHOLD);
    Serial.printf(" Debounce  : %d ms\n", DEBOUNCE_MS);
    Serial.println("-------------------------------------------");
    Serial.println(" Touch a key to test it. Raw values appear");
    Serial.println(" if CALIBRATION_PRINT_MS > 0.");
    Serial.println("===========================================\n");
}

// ─── Main Loop ───────────────────────────────────────────────────────────────

void loop() {
    uint32_t now = millis();

    for (uint8_t i = 0; i < NUM_KEYS; i++) {
        uint16_t raw     = readKey(i);
        bool     touched = (raw < TOUCH_THRESHOLD);

        // ── Rising edge: finger just placed ──
        if (touched && !keyDown[i]) {
            // Debounce: ignore if too soon after last release
            if ((now - lastReleaseMs[i]) >= DEBOUNCE_MS) {
                keyDown[i] = true;
                Serial.printf("  ▶  TOUCHED  %s  (raw=%d)\n",
                              KEYS[i].label, raw);
            }
        }

        // ── Falling edge: finger lifted ──
        if (!touched && keyDown[i]) {
            keyDown[i]       = false;
            lastReleaseMs[i] = now;
            Serial.printf("     released %s\n", KEYS[i].label);
        }
    }

#if CALIBRATION_PRINT_MS > 0
    static uint32_t lastPrint = 0;
    if (now - lastPrint >= CALIBRATION_PRINT_MS) {
        lastPrint = now;
        Serial.print("[RAW]");
        for (uint8_t i = 0; i < NUM_KEYS; i++) {
            Serial.printf("  K%d=%3d", i + 1, readKey(i));
        }
        Serial.println();
    }
#endif

    delay(10);   // ~100 Hz poll rate — plenty for human touch
}