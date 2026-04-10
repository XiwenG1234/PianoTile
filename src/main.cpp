#include <Adafruit_NeoPixel.h>
#include <TM1637Display.h>
#include <esp_timer.h>
#include <math.h>

// ─── Display Configuration ───────────────────────────────────────────────────

#define TM1637_CLK  21
#define TM1637_DIO  16

TM1637Display display(TM1637_CLK, TM1637_DIO);

// ─── LED Configuration ───────────────────────────────────────────────────────

#define NUM_KEYS          8
#define LEDS_PER_KEY      4
#define LED_PIN           17
#define LED_COUNT         (NUM_KEYS * LEDS_PER_KEY)
#define LED_BRIGHTNESS    80

// ─── Touch Configuration ─────────────────────────────────────────────────────

#define TOUCH_THRESHOLD   35
#define DEBOUNCE_MS       50

// ─── Audio Configuration ─────────────────────────────────────────────────────

#define DAC_PIN         25
#define SAMPLE_RATE     22050
#define NOTE_AMPLITUDE  100

// ─── Game Configuration ──────────────────────────────────────────────────────

#define HIT_WINDOW_MS   3000
#define POINTS_HIT      1
#define POINTS_MISS     -1

const uint8_t SONG_SEQUENCE[] = {3, 2, 5, 5, 6, 3, 4, 7, 1, 6, 3, 6, 2, 8, 4};
const int     SONG_LENGTH     = sizeof(SONG_SEQUENCE) / sizeof(SONG_SEQUENCE[0]);

const char* NOTE_NAMES[NUM_KEYS] = {
    "Do (C4)", "Re (D4)", "Mi (E4)", "Fa (F4)",
    "Sol (G4)", "La (A4)", "Ti (B4)", "Do (C5)"
};

const float KEY_FREQS[NUM_KEYS] = {
    261.63f, 293.66f, 329.63f, 349.23f,
    392.00f, 440.00f, 493.88f, 523.25f,
};

// ─── Per-key LED colours ──────────────────────────────────────────────────────

const uint8_t KEY_COLORS[NUM_KEYS][3] = {
    {255,   0,   0},
    {255, 100,   0},
    {200, 200,   0},
    {  0, 255,   0},
    {  0, 200, 200},
    {  0,   0, 255},
    {150,   0, 255},
    {255,   0, 150},
};

// ─── Pin / Key Definitions ───────────────────────────────────────────────────

struct TouchKey {
    uint8_t     gpio;
    const char* label;
};

const TouchKey KEYS[NUM_KEYS] = {
    {  4, "Key 1 (GPIO4  / T0)" },
    {  2, "Key 2 (GPIO2  / T2)" },
    { 15, "Key 3 (GPIO15 / T3)" },
    { 13, "Key 4 (GPIO13 / T4)" },
    { 12, "Key 5 (GPIO12 / T5)" },
    { 14, "Key 6 (GPIO14 / T6)" },
    { 27, "Key 7 (GPIO27 / T7)" },
    { 33, "Key 8 (GPIO33 / T8)" },
};

// ─── Shared audio state ───────────────────────────────────────────────────────

volatile bool keyActive[NUM_KEYS] = {false};

// ─── Audio state (used inside timer ISR) ─────────────────────────────────────

static float audioPhase    = 0.0f;
static float audioPhaseInc = 0.0f;

// ─── Game state ──────────────────────────────────────────────────────────────

int      score                   = 0;
int      seqIndex                = 0;
uint32_t promptStart             = 0;
bool     waitingForHit           = false;
bool     keyDown[NUM_KEYS]       = {false};
uint32_t lastReleaseMs[NUM_KEYS] = {0};

// ─── Globals ─────────────────────────────────────────────────────────────────

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// ─── Score display ────────────────────────────────────────────────────────────

void updateScore(int delta) {
    score += delta;
    if (score >= 0) {
        display.showNumberDec(score, false);
    } else {
        uint8_t segs[4];
        int abs_score = -score;
        segs[0] = SEG_G;
        segs[1] = display.encodeDigit((abs_score / 100) % 10);
        segs[2] = display.encodeDigit((abs_score / 10)  % 10);
        segs[3] = display.encodeDigit( abs_score        % 10);
        display.setSegments(segs);
    }
}

// ─── Audio timer ISR ─────────────────────────────────────────────────────────
//
// Fires at SAMPLE_RATE Hz via esp_timer.
// Computes average frequency of all active keys, outputs one DAC sample.
// No per-key phase — single accumulator, resets to 0 on silence.

void IRAM_ATTR onAudioTimer(void* arg) {
    float freqSum = 0.0f;
    int   active  = 0;

    for (int k = 0; k < NUM_KEYS; k++) {
        if (keyActive[k]) {
            freqSum += KEY_FREQS[k];
            active++;
        }
    }

    if (active > 0) {
        float avgFreq  = freqSum / (float)active;
        audioPhaseInc  = (float)M_TWOPI * avgFreq / (float)SAMPLE_RATE;
        uint8_t sample = (uint8_t)(128 + (int)(sinf(audioPhase) * NOTE_AMPLITUDE));
        dacWrite(DAC_PIN, sample);
        audioPhase += audioPhaseInc;
        if (audioPhase >= (float)M_TWOPI) audioPhase -= (float)M_TWOPI;
    } else {
        audioPhaseInc = 0.0f;
        audioPhase    = 0.0f;
        dacWrite(DAC_PIN, 128);   // silence — midpoint, no DC offset
    }
}

// ─── Audio init ──────────────────────────────────────────────────────────────

void dacInit() {
    dacWrite(DAC_PIN, 128);

    esp_timer_handle_t audioTimer;
    const esp_timer_create_args_t args = {
        .callback        = onAudioTimer,
        .arg             = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "audio"
    };
    esp_timer_create(&args, &audioTimer);
    esp_timer_start_periodic(audioTimer, 1000000 / SAMPLE_RATE);  // period in µs
}

// ─── LED Helpers ─────────────────────────────────────────────────────────────

void keyLEDOn(uint8_t i) {
    int base = i * LEDS_PER_KEY;
    uint32_t col = strip.Color(KEY_COLORS[i][0], KEY_COLORS[i][1], KEY_COLORS[i][2]);
    for (int l = 0; l < LEDS_PER_KEY; l++) strip.setPixelColor(base + l, col);
    strip.show();
}

void keyLEDOff(uint8_t i) {
    int base = i * LEDS_PER_KEY;
    for (int l = 0; l < LEDS_PER_KEY; l++) strip.setPixelColor(base + l, 0);
    strip.show();
}

void flashMiss() {
    for (int l = 0; l < LED_COUNT; l++)
        strip.setPixelColor(l, strip.Color(60, 0, 0));
    strip.show();
    delay(200);
    strip.clear();
    strip.show();
}

// ─── Game Helpers ─────────────────────────────────────────────────────────────

void silenceAll() {
    for (int k = 0; k < NUM_KEYS; k++) keyActive[k] = false;
}

void promptNext() {
    silenceAll();

    uint8_t keyIdx = SONG_SEQUENCE[seqIndex] - 1;
    promptStart    = millis();
    waitingForHit  = true;

    keyActive[keyIdx] = true;
    keyLEDOn(keyIdx);

    Serial.println();
    Serial.println("-------------------------------------------");
    Serial.printf("  [%d/%d]  Press:  KEY %d — %s\n",
                  seqIndex + 1, SONG_LENGTH,
                  SONG_SEQUENCE[seqIndex], NOTE_NAMES[keyIdx]);
    Serial.printf("  Score: %d   |   Time limit: %d ms\n", score, HIT_WINDOW_MS);
    Serial.println("-------------------------------------------");
}

void registerHit(uint8_t keyIdx) {
    uint32_t reaction = millis() - promptStart;
    updateScore(POINTS_HIT);

    keyActive[keyIdx] = true;
    keyLEDOn(keyIdx);

    Serial.printf("  +  HIT!  reaction=%d ms   score=%d\n", reaction, score);

    delay(300);

    keyActive[keyIdx] = false;
    keyLEDOff(keyIdx);

    Serial.println("  (lift finger...)");
    while (touchRead(KEYS[keyIdx].gpio) < TOUCH_THRESHOLD) {
        delay(10);
    }
    delay(DEBOUNCE_MS);

    waitingForHit = false;
    seqIndex++;
}

void registerMiss(uint8_t keyIdx) {
    silenceAll();
    keyLEDOff(keyIdx);
    updateScore(POINTS_MISS);
    flashMiss();

    Serial.printf("  x  MISS!  score=%d\n", score);

    waitingForHit = false;
    seqIndex++;
}

// ─── Setup ───────────────────────────────────────────────────────────────────

void setup() {
    delay(1000);
    Serial.begin(115200);
    Serial.println("booting...");

    Serial.println("display init...");
    display.setBrightness(5);
    display.showNumberDec(0, false);

    Serial.println("LED init...");
    strip.begin();
    strip.setBrightness(LED_BRIGHTNESS);
    strip.clear();
    strip.show();

    Serial.println("DAC init...");
    dacInit();

    Serial.println("sweep...");
    for (uint8_t i = 0; i < NUM_KEYS; i++) {
        keyActive[i] = true;
        keyLEDOn(i);
        delay(200);
        keyActive[i] = false;
        keyLEDOff(i);
        delay(30);
    }

    Serial.println("setup done.");
    delay(1000);
    promptNext();
}

// ─── Main Loop (Core 0) ──────────────────────────────────────────────────────

void loop() {
    uint32_t now = millis();

    // ── Score ticker ──────────────────────────────────────────────────────────
    static uint32_t lastScorePrint = 0;
    if (now - lastScorePrint >= 1000) {
        lastScorePrint = now;
        if (waitingForHit) {
            uint32_t remaining = HIT_WINDOW_MS - (now - promptStart);
            Serial.printf("  [score: %d]  time left: %d ms\n", score, remaining);
        }
    }

    // ── Game over ─────────────────────────────────────────────────────────────
    if (seqIndex >= SONG_LENGTH) {
        silenceAll();
        Serial.println("\n===========================================");
        Serial.println("           GAME OVER");
        Serial.printf("   Final score: %d / %d\n", score, SONG_LENGTH * POINTS_HIT);
        Serial.println("===========================================");
        display.showNumberDec(score, false);

        for (int t = 0; t < 3; t++) {
            for (int l = 0; l < LED_COUNT; l++)
                strip.setPixelColor(l, strip.Color(80, 80, 80));
            strip.show(); delay(300);
            strip.clear(); strip.show(); delay(200);
        }

        Serial.println("\n--- DEBUG MODE — press any key ---");
        Serial.println("    (raw values printed every 200 ms)");
        Serial.println("----------------------------------");

        bool dbgKeyDown[NUM_KEYS] = {false};

        while (true) {
            static uint32_t lastRaw = 0;
            uint32_t t2 = millis();
            if (t2 - lastRaw >= 200) {
                lastRaw = t2;
                Serial.print("[RAW] ");
                for (uint8_t i = 0; i < NUM_KEYS; i++)
                    Serial.printf("K%d=%3d  ", i + 1, touchRead(KEYS[i].gpio));
                Serial.println();
            }

            for (uint8_t i = 0; i < NUM_KEYS; i++) {
                uint16_t raw     = touchRead(KEYS[i].gpio);
                bool     touched = (raw < TOUCH_THRESHOLD);

                if (touched && !dbgKeyDown[i]) {
                    dbgKeyDown[i] = true;
                    keyActive[i]  = true;
                    keyLEDOn(i);
                    Serial.printf("  v  PRESS   Key %d - %s  (raw=%d)\n",
                                  i + 1, NOTE_NAMES[i], raw);
                }
                if (!touched && dbgKeyDown[i]) {
                    dbgKeyDown[i] = false;
                    keyActive[i]  = false;
                    keyLEDOff(i);
                    Serial.printf("  ^  RELEASE Key %d  (raw=%d)\n", i + 1, raw);
                }
            }

            delay(10);
        }
    }

    // ── Miss detection (timeout) ──────────────────────────────────────────────
    if (waitingForHit && (now - promptStart) >= HIT_WINDOW_MS) {
        uint8_t keyIdx = SONG_SEQUENCE[seqIndex] - 1;
        registerMiss(keyIdx);
        if (seqIndex < SONG_LENGTH) promptNext();
        return;
    }

    // ── Touch polling ─────────────────────────────────────────────────────────
    if (waitingForHit) {
        uint8_t targetKey = SONG_SEQUENCE[seqIndex] - 1;

        for (uint8_t i = 0; i < NUM_KEYS; i++) {
            uint16_t raw     = touchRead(KEYS[i].gpio);
            bool     touched = (raw < TOUCH_THRESHOLD);

            if (touched && !keyDown[i]) {
                if ((now - lastReleaseMs[i]) >= DEBOUNCE_MS) {
                    keyDown[i] = true;

                    if (i == targetKey) {
                        registerHit(i);
                        if (seqIndex < SONG_LENGTH) promptNext();
                        return;
                    } else {
                        Serial.printf("  x  Wrong key %d (need %d)\n",
                                      i + 1, targetKey + 1);
                    }
                }
            }

            if (!touched && keyDown[i]) {
                keyDown[i]       = false;
                lastReleaseMs[i] = now;
            }
        }
    }

    delay(10);
}