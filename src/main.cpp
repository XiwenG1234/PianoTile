#include <Adafruit_NeoPixel.h>
#include <math.h>

#define LED_PIN     19
#define LED_COUNT   100
#define BRIGHTNESS  200

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

const int ledMatrix[4][8] = {
  {0,17,21,38,42,59,63,80},
  {2,15,23,36,44,57,65,78},
  {4,13,25,34,46,55,67,76},
  {6,11,27,32,48,53,69,74}
};

struct Note {
  int col;
  float pos;
  bool active;
  int hitState;
};

#define MAX_NOTES 30
Note notes[MAX_NOTES];

const int TOUCH_PINS[8] = {4,2,15,33,27,14,12,13};
const int TOUCH_THRESHOLD = 35;

unsigned long lastMove  = 0;
unsigned long lastSpawn = 0;

const int   moveInterval  = 30;
const int   spawnInterval = 800;
const float speed         = 0.04f;

const float HIT_START = 2.0f;
const float HIT_END   = 4.0f;

// ★ NEW: timestamp per column for wrong-press red flash
unsigned long wrongPressTime[8] = {0};
const unsigned long WRONG_FLASH_MS = 300;

void allOff() {
  for (int i = 0; i < LED_COUNT; i++) strip.setPixelColor(i, 0);
}

void spawnNotes() {
  int count = random(1, 3);
  for (int i = 0; i < count; i++) {
    int col = random(0, 8);
    for (int j = 0; j < MAX_NOTES; j++) {
      if (!notes[j].active) {
        notes[j].active   = true;
        notes[j].col      = col;
        notes[j].pos      = 0.0f;
        notes[j].hitState = 0;
        break;
      }
    }
  }
}

void updateNotes() {
  for (int i = 0; i < MAX_NOTES; i++) {
    if (!notes[i].active) continue;
    notes[i].pos += speed;
    if (notes[i].hitState == 0 && notes[i].pos > HIT_END) {
      notes[i].hitState = 2;
    }
    if (notes[i].pos > 4.5f) notes[i].active = false;
  }
}

void checkTouch() {
  bool touched[8] = {false};
  for (int c = 0; c < 8; c++) {
    if (touchRead(TOUCH_PINS[c]) < TOUCH_THRESHOLD) touched[c] = true;
  }

  for (int i = 0; i < MAX_NOTES; i++) {
    if (!notes[i].active || notes[i].hitState != 0) continue;
    if (notes[i].pos < HIT_START || notes[i].pos > HIT_END) continue;

    int target = notes[i].col;
    if (touched[target]) notes[i].hitState = 1;
    for (int c = 0; c < 8; c++) {
      if (c != target && touched[c]) notes[i].hitState = 2;
    }
  }

  // ★ FIXED: record timestamp instead of writing pixel directly
  for (int c = 0; c < 8; c++) {
    if (!touched[c]) continue;
    bool hasHittable = false;
    for (int i = 0; i < MAX_NOTES; i++) {
      if (!notes[i].active) continue;
      if (notes[i].col != c) continue;
      if (notes[i].pos >= HIT_START && notes[i].pos <= HIT_END) {
        hasHittable = true;
        break;
      }
    }
    if (!hasHittable) wrongPressTime[c] = millis();
  }

  delay(20);
}

void render() {
  allOff();

  for (int i = 0; i < MAX_NOTES; i++) {
    if (!notes[i].active) continue;

    int   col = notes[i].col;
    float p   = notes[i].pos;
    int   hit = notes[i].hitState;

    for (int row = 0; row < 4; row++) {
      float delta = p - row;
      uint8_t bri = 0;
      if (delta >= -1.0f && delta <= 1.0f) {
        bri = (uint8_t)(255.0f * (1.0f - fabs(delta)));
      }
      if (bri == 0) continue;

      int led = ledMatrix[row][col];
      if (row == 3) {
        if      (hit == 1) strip.setPixelColor(led, 0,   bri, 0  );
        else if (hit == 2) strip.setPixelColor(led, bri, 0,   0  );
        else               strip.setPixelColor(led, bri, bri, bri);
      } else {
        strip.setPixelColor(led, bri, bri, bri);
      }
    }
  }

  // ★ FIXED: draw wrong-press red last so it overwrites everything
  unsigned long now = millis();
  for (int c = 0; c < 8; c++) {
    if (wrongPressTime[c] > 0 && (now - wrongPressTime[c]) < WRONG_FLASH_MS) {
      strip.setPixelColor(ledMatrix[3][c], 255, 0, 0);
    }
  }

  strip.show();
}

void setup() {
  strip.begin();
  strip.setBrightness(BRIGHTNESS);
  strip.clear();
  strip.show();
  randomSeed(analogRead(A0));
  for (int i = 0; i < MAX_NOTES; i++) notes[i].active = false;
}

void loop() {
  unsigned long now = millis();

  if (now - lastMove >= moveInterval) {
    lastMove = now;
    updateNotes();
  }
  if (now - lastSpawn >= spawnInterval) {
    lastSpawn = now;
    spawnNotes();
  }

  checkTouch();
  render();
}