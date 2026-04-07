/*
 * Piano Tiles — Touch + LED + Audio Test
 * ESP32-DEVKITC-32UE
 *
 * Touch pins:
 *   GPIO4  → Key 1   GPIO13 → Key 4
 *   GPIO2  → Key 2   GPIO12 → Key 5
 *   GPIO15 → Key 3   GPIO14 → Key 6
 *   GPIO27 → Key 7   GPIO33 → Key 8
 *
 * LEDs:  GPIO17 → WS2812B DIN, 32 LEDs (4 per key)
 *
 * Audio: MAX98357A I2S amplifier
 *   GPIO25 → LRCLK (LRC / WS)
 *   GPIO26 → BCLK
 *   GPIO22 → DIN
 *
 * Each key plays a note of the C major scale (C4–C5).
 * Touch → LED on + note plays for 300 ms.
 * Release has no effect on audio (note already finished).
 */

#include <Adafruit_NeoPixel.h>
#include <driver/i2s.h>

// ─── LED Configuration ───────────────────────────────────────────────────────

#define NUM_KEYS          8
#define LEDS_PER_KEY      4
#define LED_PIN           17
#define LED_COUNT         (NUM_KEYS * LEDS_PER_KEY)
#define LED_BRIGHTNESS    80

// ─── Touch Configuration ─────────────────────────────────────────────────────

#define TOUCH_THRESHOLD   40
#define DEBOUNCE_MS       200
#define CALIBRATION_PRINT_MS 0

// ─── Audio Configuration ─────────────────────────────────────────────────────

#define I2S_NUM         I2S_NUM_0
#define I2S_LRCLK       25
#define I2S_BCLK        26
#define I2S_DOUT        22

#define SAMPLE_RATE     44100
#define NOTE_DURATION_MS  300      // how long each note plays
#define NOTE_AMPLITUDE  32000     // 0–32767; raise for louder, lower for quieter

// C major scale: C4 D4 E4 F4 G4 A4 B4 C5
const float KEY_FREQS[NUM_KEYS] = {
    261.63f,   // C4 — Key 1
    293.66f,   // D4 — Key 2
    329.63f,   // E4 — Key 3
    349.23f,   // F4 — Key 4
    392.00f,   // G4 — Key 5
    440.00f,   // A4 — Key 6
    493.88f,   // B4 — Key 7
    523.25f,   // C5 — Key 8
};

// ─── Per-key LED colours ──────────────────────────────────────────────────────

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
        .dma_buf_len          = 64,
        .use_apll             = false,
        .tx_desc_auto_clear   = true,
    };

    i2s_pin_config_t pins = {
        .bck_io_num   = I2S_BCLK,
        .ws_io_num    = I2S_LRCLK,
        .data_out_num = I2S_DOUT,
        .data_in_num  = I2S_PIN_NO_CHANGE,
    };

    i2s_driver_install(I2S_NUM, &cfg, 0, NULL);
    i2s_set_pin(I2S_NUM, &pins);
    i2s_zero_dma_buffer(I2S_NUM);
}

// ─── Audio: Play a sine-wave tone ────────────────────────────────────────────
// Generates samples in a small stack buffer and writes them to I2S.
// Blocks for NOTE_DURATION_MS ms then returns.

void playNote(float freqHz) {
    const int totalSamples = (SAMPLE_RATE * NOTE_DURATION_MS) / 1000;
    const int BUF_SAMPLES  = 256;          // samples per write chunk
    int16_t   buf[BUF_SAMPLES * 2];        // stereo: L + R interleaved

    int written = 0;
    float phase = 0.0f;
    float phaseInc = 2.0f * 3.14159265f * freqHz / (float)SAMPLE_RATE;

    // Simple linear fade-out envelope to avoid clicks at note end
    while (written < totalSamples) {
        int chunk = min(BUF_SAMPLES, totalSamples - written);
        for (int s = 0; s < chunk; s++) {
            // Fade out over last 20% of the note
            float env = 1.0f;
            int remaining = totalSamples - written - s;
            int fadeLen   = totalSamples / 5;
            if (remaining < fadeLen) {
                env = (float)remaining / (float)fadeLen;
            }

            int16_t sample = (int16_t)(NOTE_AMPLITUDE * sinf(phase) * env);
            buf[s * 2]     = sample;   // Left
            buf[s * 2 + 1] = sample;   // Right
            phase += phaseInc;
            if (phase > 2.0f * 3.14159265f) phase -= 2.0f * 3.14159265f;
        }

        size_t bytesWritten = 0;
        i2s_write(I2S_NUM, buf, chunk * 2 * sizeof(int16_t),
                  &bytesWritten, portMAX_DELAY);
        written += chunk;
    }
}

// ─── LED Helpers ─────────────────────────────────────────────────────────────

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

    // LED strip init
    strip.begin();
    strip.setBrightness(LED_BRIGHTNESS);
    strip.clear();
    strip.show();

    // I2S / audio init
    i2sInit();

    // Startup sweep — lights + a quick ascending scale
    for (uint8_t i = 0; i < NUM_KEYS; i++) {
        keyLEDOn(i);
        playNote(KEY_FREQS[i]);   // NOTE_DURATION_MS each
        keyLEDOff(i);
    }

    delay(500);

    Serial.println("===========================================");
    Serial.println(" Piano Tiles — Touch + LED + Audio Test");
    Serial.println("===========================================");
    Serial.printf(" Threshold  : %d  (lower = touched)\n", TOUCH_THRESHOLD);
    Serial.printf(" Debounce   : %d ms\n", DEBOUNCE_MS);
    Serial.printf(" LEDs/key   : %d  (%d total on GPIO%d)\n",
                  LEDS_PER_KEY, LED_COUNT, LED_PIN);
    Serial.println(" Audio      : MAX98357A on GPIO25/26/22");
    Serial.println("-------------------------------------------");
    Serial.println(" Touch a key → LED on + note plays.");
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
                Serial.printf("  ▶  TOUCHED  %s  (raw=%d)\n",
                              KEYS[i].label, raw);
                playNote(KEY_FREQS[i]);   // blocks for NOTE_DURATION_MS
                keyLEDOff(i);
            }
        }

        // Falling edge — finger lifted
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
            Serial.printf("  K%d=%3d", i + 1, touchRead(KEYS[i].gpio));
        }
        Serial.println();
    }
#endif

    delay(10);
}