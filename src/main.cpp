
#include <Adafruit_NeoPixel.h>
#include <TM1637Display.h>
#include <driver/i2s.h>
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

#define I2S_NUM_PORT    I2S_NUM_0
#define I2S_LRCLK       25
#define I2S_BCLK        26
#define I2S_DOUT        22

#define SAMPLE_RATE     44100
#define NOTE_AMPLITUDE  28000
#define FADE_SAMPLES    882        // ~20 ms fade at 44100 Hz

// ─── Game Configuration ───────────────────────────────────────────────────────

#define HIT_WINDOW_MS   3000
#define POINTS_HIT      1
#define POINTS_MISS     -1
#define CALIBRATION_PRINT_MS 0

const uint8_t SONG_SEQUENCE[] = {3, 2, 5, 5, 6, 3, 4, 7, 1, 6, 3, 6, 2, 8, 4};
const int     SONG_LENGTH     = sizeof(SONG_SEQUENCE) / sizeof(SONG_SEQUENCE[0]);

const char* NOTE_NAMES[NUM_KEYS] = {
    "Do (C4)", "Re (D4)", "Mi (E4)", "Fa (F4)",
    "Sol (G4)","La (A4)", "Ti (B4)", "Do (C5)"
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
    {  4, "Key 1 (GPIO4 / T0)"  },
    {  2, "Key 2 (GPIO2 / T2)"  },
    { 15, "Key 3 (GPIO15 / T3)" },
    { 33, "Key 4 (GPIO13 / T4)" },
    { 27, "Key 5 (GPIO12 / T5)" },
    { 14, "Key 6 (GPIO14 / T6)" },
    { 12, "Key 7 (GPIO27 / T7)" },
    { 13, "Key 8 (GPIO33 / T8)" },
};

// ─── Shared audio state ───────────────────────────────────────────────────────

volatile bool keyActive[NUM_KEYS] = {false};

// ─── Game state ──────────────────────────────────────────────────────────────

int      score                   = 0;
int      seqIndex                = 0;
uint32_t promptStart             = 0;
bool     waitingForHit           = false;
bool     keyDown[NUM_KEYS]       = {false};
uint32_t lastReleaseMs[NUM_KEYS] = {0};

// ─── Globals ─────────────────────────────────────────────────────────────────

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// ─── Score display — defined first so all callers can use it ─────────────────

void updateScore(int delta) {
    score += delta;
    if (score >= 0) {
        display.showNumberDec(score, false);
    } else {
        // Leftmost digit = minus sign, remaining 3 digits = absolute value
        uint8_t segs[4];
        int abs_score = -score;
        segs[0] = SEG_G;
        segs[1] = display.encodeDigit((abs_score / 100) % 10);
        segs[2] = display.encodeDigit((abs_score / 10)  % 10);
        segs[3] = display.encodeDigit( abs_score        % 10);
        display.setSegments(segs);
    }
}

// ─── I2S Setup ───────────────────────────────────────────────────────────────

void i2sInit() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 128,
        .use_apll             = false,
        .tx_desc_auto_clear   = true,
    };
    i2s_pin_config_t pins = {
        .bck_io_num   = I2S_BCLK,
        .ws_io_num    = I2S_LRCLK,
        .data_out_num = I2S_DOUT,
        .data_in_num  = I2S_PIN_NO_CHANGE,
    };
    i2s_driver_install(I2S_NUM_PORT, &cfg, 0, NULL);
    i2s_set_pin(I2S_NUM_PORT, &pins);
    i2s_zero_dma_buffer(I2S_NUM_PORT);
}

// ─── Audio Task (Core 1) ──────────────────────────────────────────────────────
void audioTask(void* param) {
    const int   BUF_SAMPLES = 128;
    int16_t buf[BUF_SAMPLES * 2];
    float avgPhase    = 0.0f;
    float avgPhaseInc = 0.0f;

    while (true) {
        // Compute average frequency of all active keys
        float freqSum = 0.0f;
        int   active  = 0;
        for (int k = 0; k < NUM_KEYS; k++) {
            if (keyActive[k]) {
                freqSum += KEY_FREQS[k];
                active++;
            }
        }

        if (active > 0) {
            float avgFreq = freqSum / active;
            avgPhaseInc   = M_TWOPI * avgFreq / (float)SAMPLE_RATE;
        } else {
            avgPhaseInc = 0.0f;
        }

        for (int i = 0; i < BUF_SAMPLES; i++) {
            int16_t out = 0;
            if (active > 0) {
                out = (int16_t)(sinf(avgPhase) * NOTE_AMPLITUDE);
                avgPhase += avgPhaseInc;
                if (avgPhase > M_TWOPI) avgPhase -= M_TWOPI;
            } else {
                avgPhase = 0.0f;   // reset when silent — no residual buildup
            }
            buf[i * 2]     = out;
            buf[i * 2 + 1] = out;
        }

        size_t bytesWritten;
        i2s_write(I2S_NUM_PORT, buf, sizeof(buf), &bytesWritten, portMAX_DELAY);
    }
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
    for (int l = 0; l < LED_COUNT; l++) strip.setPixelColor(l, strip.Color(60, 0, 0));
    strip.show();
    delay(200);
    strip.clear();
    strip.show();
}

// ─── Game Helpers ─────────────────────────────────────────────────────────────

void promptNext() {
    for (int k = 0; k < NUM_KEYS; k++) keyActive[k] = false;  // silence all

    uint8_t keyIdx = SONG_SEQUENCE[seqIndex] - 1;
    promptStart   = millis();
    waitingForHit = true;
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

    Serial.printf("  ✓  HIT!  reaction=%d ms   score=%d\n", reaction, score);

    delay(300);
    keyActive[keyIdx] = false;
    keyLEDOff(keyIdx);

    // Wait for finger to lift
    Serial.println("  (lift finger...)");
    while (touchRead(KEYS[keyIdx].gpio) < TOUCH_THRESHOLD) {
        delay(10);
    }
    delay(DEBOUNCE_MS);

    waitingForHit = false;
    seqIndex++;
}

void registerMiss(uint8_t keyIdx) {
    updateScore(POINTS_MISS);         // updates score + display
    keyLEDOff(keyIdx);
    flashMiss();

    Serial.printf("  ✗  MISS!  score=%d\n", score);

    waitingForHit = false;
    seqIndex++;
}

// ─── Setup ───────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);

    display.setBrightness(5);
    display.showNumberDec(0, false);

    strip.begin();
    strip.setBrightness(LED_BRIGHTNESS);
    strip.clear();
    strip.show();

    i2sInit();
    xTaskCreatePinnedToCore(audioTask, "audio", 4096, NULL, 5, NULL, 1);

    // Startup sweep
    for (uint8_t i = 0; i < NUM_KEYS; i++) {
        keyActive[i] = true;
        keyLEDOn(i);
        delay(200);
        keyActive[i] = false;
        keyLEDOff(i);
        delay(30);
    }

    Serial.println("===========================================");
    Serial.println("       Piano Tiles — Game Mode");
    Serial.println("===========================================");
    Serial.printf(" Sequence length : %d keys\n", SONG_LENGTH);
    Serial.printf(" Hit window      : %d ms\n",   HIT_WINDOW_MS);
    Serial.printf(" Hit reward      : +%d\n",     POINTS_HIT);
    Serial.printf(" Miss penalty    : %d\n",      POINTS_MISS);
    Serial.println("===========================================");
    delay(1000);

    promptNext();
}

// ─── Main Loop (Core 0) ──────────────────────────────────────────────────────

void loop() {
    uint32_t now = millis();

    // ── Score ticker: Serial print once per second ────────────────────────────
    static uint32_t lastScorePrint = 0;
    if (now - lastScorePrint >= 1000) {
        lastScorePrint = now;
        if (waitingForHit) {
            uint32_t remaining = HIT_WINDOW_MS - (now - promptStart);
            Serial.printf("  [score: %d]  time left: %d ms\n", score, remaining);
        }
    }

    // ── Game over ─────────────────────────────────────────────────────────────
    // ── Game over ─────────────────────────────────────────────────────────────
if (seqIndex >= SONG_LENGTH) {
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

    // ── Debug loop — print pressed keys in real time ──────────────────────
    Serial.println("\n--- DEBUG MODE — press any key ---");
    Serial.println("    (raw values printed every 200 ms)");
    Serial.println("----------------------------------");

    bool dbgKeyDown[NUM_KEYS] = {false};

    while (true) {
        // Raw value dump every 200 ms
        static uint32_t lastRaw = 0;
        uint32_t t2 = millis();
        if (t2 - lastRaw >= 200) {
            lastRaw = t2;
            Serial.print("[RAW] ");
            for (uint8_t i = 0; i < NUM_KEYS; i++) {
                Serial.printf("K%d=%3d  ", i + 1, touchRead(KEYS[i].gpio));
            }
            Serial.println();
        }

        // Edge detection — print on press and release
        for (uint8_t i = 0; i < NUM_KEYS; i++) {
            uint16_t raw     = touchRead(KEYS[i].gpio);
            bool     touched = (raw < TOUCH_THRESHOLD);

            if (touched && !dbgKeyDown[i]) {
                dbgKeyDown[i] = true;
                keyActive[i]  = true;   // play the note
                keyLEDOn(i);
                Serial.printf("  ▼  PRESS   Key %d — %s  (raw=%d)\n",
                              i + 1, NOTE_NAMES[i], raw);
            }

            if (!touched && dbgKeyDown[i]) {
                dbgKeyDown[i] = false;
                keyActive[i]  = false;  // stop the note
                keyLEDOff(i);
                Serial.printf("  ▲  RELEASE Key %d  (raw=%d)\n",
                              i + 1, raw);
            }
        }

        delay(10);
    }
}

    // ── Miss detection ────────────────────────────────────────────────────────
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
                        Serial.printf("  ✗  Wrong key %d (need %d)\n",
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

    // TEMP: calibration print — remove after tuning
    static uint32_t lastCal = 0;
    if (now - lastCal >= 500) {
        lastCal = now;
        Serial.print("[RAW] ");
        for (uint8_t i = 0; i < NUM_KEYS; i++) {
            Serial.printf("K%d=%3d  ", i + 1, touchRead(KEYS[i].gpio));
        }
        Serial.println();
    }

    delay(10);
}
    
   /*
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

#define LED_PIN   19
#define LED_COUNT 100

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

void setup() {
    strip.begin();
    strip.setBrightness(80);
    strip.clear();

    int leds[] = {0, 2, 4, 6, 11, 13, 15, 17, 21, 23, 25, 27, 32, 34, 36, 38, 42, 44, 46, 48, 53, 55, 57, 59, 63, 65, 67, 69, 74, 76, 78, 80};
    int count = sizeof(leds) / sizeof(leds[0]);

    for (int i = 0; i < count; i++) {
        strip.setPixelColor(leds[i], strip.Color(255, 255, 255));
    }
    strip.show();
}

void loop() {
    // Nothing needed here, LEDs stay on from setup()
} 
    */