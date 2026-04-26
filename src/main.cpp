#include <Adafruit_NeoPixel.h>
#include <math.h>
#include <TM1637Display.h>
#include <driver/i2s.h> 

#define LED_PIN     17  
#define LED_COUNT   100
#define BRIGHTNESS  200

#define CLK 0
#define DIO 16
TM1637Display display(CLK, DIO);

#define I2S_NUM_PORT    I2S_NUM_0
#define I2S_LRCLK       25
#define I2S_BCLK        26
#define I2S_DOUT        22
#define SAMPLE_RATE     44100
#define NOTE_AMPLITUDE  30000 

void render();
void updateScoreDisplay();
void playTone(float freq, int dur);
void initFourNotes();
void checkGameState();

enum GameState { STATE_MENU, STATE_PLAYING, STATE_WIN, STATE_GAMEOVER };
GameState gameState = STATE_MENU;

int score = 0;
int maxScoreReached = 0; 
unsigned long stateStartTime = 0;

const float KEY_FREQS[8] = {
  261.63f, 293.66f, 329.63f, 349.23f, // Do, Re, Mi, Fa
  392.00f, 440.00f, 493.88f, 523.25f, // Sol, La, Ti, Do
};

const int S1[] = {1,1,5,5,6,6,5, 4,4,3,3,2,2,1}; 
const int S2[] = {3,3,4,5,5,4,3,2, 1,1,2,3,3,2,2}; 
const int S3[] = {3,2,1,2,3,3,3, 2,2,2, 3,5,5}; 
const int S4[] = {5,6,5,4,3,4,5, 2,3,4, 3,4,5};
const int S5[] = {3,3,3, 3,3,3, 3,5,1,2,3}; 
const int S6[] = {1,2,3,1, 1,2,3,1, 3,4,5, 3,4,5}; 
const int S7[] = {1,1,1,2,3, 3,2,3,4,5}; 
const int S8[] = {5,3,3, 4,2,2, 1,2,3,4,5,5,5}; 

const int* SONGS[8] = {S1, S2, S3, S4, S5, S6, S7, S8};
const int SONG_LENGTHS[8] = {
  sizeof(S1)/sizeof(int), sizeof(S2)/sizeof(int), sizeof(S3)/sizeof(int), sizeof(S4)/sizeof(int),
  sizeof(S5)/sizeof(int), sizeof(S6)/sizeof(int), sizeof(S7)/sizeof(int), sizeof(S8)/sizeof(int)
};

int currentSong = 0;
int meledyPtr = 0;

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

const int ledMatrix[4][8] = {
  {0,17,21,38,42,59,63,80},
  {2,15,23,36,44,57,65,78},
  {4,13,25,34,46,55,67,76},
  {6,11,27,32,48,53,69,74}
};

struct Note {
  int col; float pos; bool active; int hitState;
};

#define MAX_NOTES 30
Note notes[MAX_NOTES];

const int TOUCH_PINS[8] = {4,2,15,33,27,14,12,13};
const int TOUCH_THRESHOLD = 35;

bool keyJustPressed[8] = {false};

unsigned long lastMove = 0;
const int   moveInterval  = 30;
const float speed         = 0.065f;
const float HIT_START     = 2.0f;
const float HIT_END       = 4.0f;

unsigned long wrongPressTime[8] = {0};
const unsigned long WRONG_FLASH_MS = 300;

volatile float targetFreq = 0.0f;
volatile uint32_t soundStopTime = 0;


uint8_t flipSegment(uint8_t seg) {
  uint8_t flipped = 0;
  if (seg & 0x01) flipped |= 0x08; 
  if (seg & 0x02) flipped |= 0x10; 
  if (seg & 0x04) flipped |= 0x20; 
  if (seg & 0x08) flipped |= 0x01;
  if (seg & 0x10) flipped |= 0x02;
  if (seg & 0x20) flipped |= 0x04; 
  if (seg & 0x40) flipped |= 0x40; 
  return flipped;
}

void showNumberUpsideDown(int val) {
  uint8_t segments[4] = {0, 0, 0, 0};
  
  if (val == 0) {
    segments[0] = flipSegment(display.encodeDigit(0));
  } else {
    int temp = val;
    for (int i = 0; i < 4; i++) {
      if (temp > 0) {
        segments[i] = flipSegment(display.encodeDigit(temp % 10));
        temp /= 10;
      }
    }
  }
  display.setSegments(segments);
}

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
        .bck_io_num = I2S_BCLK, .ws_io_num = I2S_LRCLK, 
        .data_out_num = I2S_DOUT, .data_in_num = I2S_PIN_NO_CHANGE,
    };
    i2s_driver_install(I2S_NUM_PORT, &cfg, 0, NULL);
    i2s_set_pin(I2S_NUM_PORT, &pins);
    i2s_zero_dma_buffer(I2S_NUM_PORT);
}

void audioTask(void* param) {
    const int BUF_SAMPLES = 128;
    int16_t buf[BUF_SAMPLES * 2];
    float phase = 0.0f;
    while (true) {
        float phaseInc = 0.0f;
        if (millis() < soundStopTime && targetFreq > 0) {
            phaseInc = (2.0f * M_PI * targetFreq) / (float)SAMPLE_RATE;
        } else {
            phase = 0.0f;
        }
        for (int i = 0; i < BUF_SAMPLES; i++) {
            int16_t out = 0;
            if (phaseInc > 0) {
                out = (int16_t)(sinf(phase) * NOTE_AMPLITUDE);
                phase += phaseInc;
                if (phase > 2.0f * M_PI) phase -= 2.0f * M_PI;
            }
            buf[i * 2] = out; buf[i * 2 + 1] = out;
        }
        size_t bw;
        i2s_write(I2S_NUM_PORT, buf, sizeof(buf), &bw, portMAX_DELAY);
    }
}

void playTone(float freq, int dur) {
  targetFreq = freq; soundStopTime = millis() + dur; 
}

void playNextSongNote() {
  int noteNum = SONGS[currentSong][meledyPtr];
  playTone(KEY_FREQS[noteNum - 1], 120); 
  meledyPtr = (meledyPtr + 1) % SONG_LENGTHS[currentSong];
}

void updateScoreDisplay() {
  if (score >= 0) {
    showNumberUpsideDown(score);
  }
}

void allOff() {
  for (int i=0; i<LED_COUNT; i++) strip.setPixelColor(i, 0);
}

void initFourNotes() {
  for (int i=0; i<MAX_NOTES; i++) notes[i].active = false;
  for (int i=0; i<4; i++) {
    notes[i].col = random(8);
    notes[i].pos = 3.0f - i;
    notes[i].active = true;
    notes[i].hitState = 0;
  }
}

void readInputs() {
  static bool lastState[8] = {false};
  for(int i=0; i<8; i++) {
    bool current = (touchRead(TOUCH_PINS[i]) < TOUCH_THRESHOLD);
    keyJustPressed[i] = (current && !lastState[i]); 
    lastState[i] = current;
  }
}

void checkGameState() {
  if (score > maxScoreReached) {
    maxScoreReached = score;
  }

  if (score >= 150) { 
    gameState = STATE_WIN;
    stateStartTime = millis();
    playTone(KEY_FREQS[0], 100); delay(100);
    playTone(KEY_FREQS[2], 100); delay(100);
    playTone(KEY_FREQS[4], 100); delay(100);
    playTone(KEY_FREQS[7], 400); 
  } 
  else if (score < 0) {
    if (maxScoreReached >= 10) { 
      gameState = STATE_GAMEOVER;
      stateStartTime = millis();
      playTone(KEY_FREQS[7], 150); delay(150);
      playTone(KEY_FREQS[4], 150); delay(150);
      playTone(KEY_FREQS[2], 150); delay(150);
      playTone(KEY_FREQS[0], 400);
    } else {
      score = 0;
      updateScoreDisplay();
    }
  }
}

void updateMenu() {
  allOff();
  
  int pulse = (sin(millis() / 250.0) * 80) + 175; 
  for(int i=0; i<8; i++) {
    strip.setPixelColor(ledMatrix[3][i], strip.Color(pulse, pulse / 12, pulse / 5));
  }
  strip.show();

  uint8_t data[] = { 0x40, 0x40, 0x40, 0x40 }; 
  display.setSegments(data);

  for(int c=0; c<8; c++) {
    if (keyJustPressed[c]) {
      currentSong = c; 
      meledyPtr = 0; 
      score = 0;
      maxScoreReached = 0;
      updateScoreDisplay(); 
      initFourNotes();
      gameState = STATE_PLAYING; 
      lastMove = millis();
      playTone(KEY_FREQS[SONGS[c][0]-1], 150);
      delay(500);
      return;
    }
  }
}

void updateWin() {
  showNumberUpsideDown(150); 
  
  unsigned long now = millis();
  for(int i=0; i<strip.numPixels(); i++) {
    long hue = 60000 + sin((now / 200.0) + (i * 0.3)) * 3000; 
    float flash = (sin((now / 100.0) + (i * 0.6)) * 0.5) + 0.5; 
    uint8_t val = 100 + (155 * flash); 
    strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(hue, 230, val)));
  }
  strip.show();

  if (now - stateStartTime > 4000) { 
    gameState = STATE_MENU;
  }
}

void updateGameOver() {
  showNumberUpsideDown(0); 
  allOff();
  int flash = (millis() % 400 < 200) ? 150 : 0;
  for(int i=0; i<LED_COUNT; i++) strip.setPixelColor(i, strip.Color(flash, 0, 0));
  strip.show();
  if (millis() - stateStartTime > 3000) gameState = STATE_MENU;
}

void updateNotes() {
  int cnt = 0;
  for (int i=0; i<MAX_NOTES; i++) {
    if (!notes[i].active) continue;
    notes[i].pos += speed;
    if (notes[i].hitState == 0 && notes[i].pos > HIT_END) {
      notes[i].hitState = 2; wrongPressTime[notes[i].col] = millis();
      score -= 1; updateScoreDisplay(); checkGameState();
    }
    if (notes[i].pos > 4.5f) { notes[i].active = false; }
  }
  for (int i=0; i<MAX_NOTES; i++) if (notes[i].active) cnt++;
  if (cnt < 4) {
    for (int i=0; i<MAX_NOTES; i++) {
      if (!notes[i].active) {
        notes[i].col = random(8); notes[i].pos = 0.0f;
        notes[i].active = true; notes[i].hitState = 0; break;
      }
    }
  }
}

void checkTouch() {
  for (int i=0; i<MAX_NOTES; i++) {
    if (!notes[i].active || notes[i].hitState != 0) continue;
    if (notes[i].pos < HIT_START || notes[i].pos > HIT_END) continue;

    int target = notes[i].col;
    if (keyJustPressed[target]) {
      notes[i].hitState = 1;
      score += 2;             
      updateScoreDisplay();   
      checkGameState();

      playNextSongNote();
      keyJustPressed[target] = false; 

      render();   
      delay(80);  
      
      if (gameState != STATE_PLAYING) return;

    } else {
      for (int c=0; c<8; c++) {
        if (keyJustPressed[c]) {
          notes[i].hitState = 2; wrongPressTime[c] = millis();
          score -= 1; updateScoreDisplay(); checkGameState();
          keyJustPressed[c] = false;     
        }
      }
    }
  }
  for (int c=0; c<8; c++) {
    if (keyJustPressed[c]) {
      wrongPressTime[c] = millis();
      score -= 1; updateScoreDisplay(); checkGameState();
    }
  }
}

void render() {
  allOff();
  for (int i=0; i<MAX_NOTES; i++) {
    if (!notes[i].active) continue;
    int col = notes[i].col; float p = notes[i].pos; int hit = notes[i].hitState;
    for (int row=0; row<4; row++) {
      float d = p - row; uint8_t b = 0;
      if (d >= -1.0f && d <= 1.0f) b = 255 * (1.0f - fabs(d));
      if (b == 0) continue;
      int led = ledMatrix[row][col];
      if (row == 3) {
        if (hit == 1) strip.setPixelColor(led, 0, b, 0);
        else if (hit == 2) strip.setPixelColor(led, b, 0, 0);
        else strip.setPixelColor(led, b, b, b);
      } else { strip.setPixelColor(led, b, b, b); }
    }
  }
  unsigned long now = millis();
  for (int c=0; c<8; c++) {
    if (wrongPressTime[c] > 0 && (now - wrongPressTime[c]) < WRONG_FLASH_MS) {
      strip.setPixelColor(ledMatrix[3][c], 255, 0, 0);
    }
  }
  strip.show();
}

void setup() {
  strip.begin(); strip.setBrightness(BRIGHTNESS); strip.clear(); strip.show();
  display.setBrightness(0x0f); 
  
  i2sInit();
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, NULL, 5, NULL, 1);
  randomSeed(analogRead(A0));
  
  gameState = STATE_MENU; 
}

void loop() {
  readInputs(); 

  if (gameState == STATE_MENU) {
    updateMenu();
  } 
  else if (gameState == STATE_PLAYING) {
    unsigned long now = millis();
    if (now - lastMove >= moveInterval) {
      lastMove = now;
      updateNotes();
    }
    checkTouch();
    
    if (gameState == STATE_PLAYING) {
      render();
    }
  } 
  else if (gameState == STATE_WIN) {
    updateWin();
  }
  else if (gameState == STATE_GAMEOVER) {
    updateGameOver();
  }
}