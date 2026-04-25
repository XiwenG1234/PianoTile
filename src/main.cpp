
#include <Adafruit_NeoPixel.h>
#include <math.h>
#include <TM1637Display.h>
#include <driver/i2s.h> 

#define LED_PIN     17
#define LED_COUNT   100
#define BRIGHTNESS  200

#define CLK 21
#define DIO 5
TM1637Display display(CLK, DIO);

// I2S 放大器引脚配置
#define I2S_NUM_PORT    I2S_NUM_0
#define I2S_LRCLK       25
#define I2S_BCLK        26
#define I2S_DOUT        22
#define SAMPLE_RATE     44100
#define NOTE_AMPLITUDE  20000 

// ─── 提前声明函数 ────────────────────────────────────────────────────────
void render();
void updateScoreDisplay();
void playTone(float freq, int dur);
void initFourNotes();

// ─── 游戏状态机定义 ──────────────────────────────────────────────────────
enum GameState { STATE_MENU, STATE_PLAYING, STATE_WIN };
GameState gameState = STATE_MENU;

int score = 0;
unsigned long winStartTime = 0;

// ─── 音符频率定义 ────────────────────────────────────────────────────────
const float KEY_FREQS[8] = {
  261.63f, 293.66f, 329.63f, 349.23f, // Do, Re, Mi, Fa
  392.00f, 440.00f, 493.88f, 523.25f, // Sol, La, Ti, Do(高)
};

// ─── 8首曲库定义 (1-8 对应不同音高) ──────────────────────────────────────
const int S1[] = {1,1,5,5,6,6,5, 4,4,3,3,2,2,1}; // 1. 小星星
const int S2[] = {3,3,4,5,5,4,3,2, 1,1,2,3,3,2,2}; // 2. 欢乐颂
const int S3[] = {3,2,1,2,3,3,3, 2,2,2, 3,5,5}; // 3. 玛丽有只小绵羊
const int S4[] = {5,6,5,4,3,4,5, 2,3,4, 3,4,5}; // 4. 伦敦桥
const int S5[] = {3,3,3, 3,3,3, 3,5,1,2,3}; // 5. 铃儿响叮当
const int S6[] = {1,2,3,1, 1,2,3,1, 3,4,5, 3,4,5}; // 6. 两只老虎
const int S7[] = {1,1,1,2,3, 3,2,3,4,5}; // 7. 划小船
const int S8[] = {5,3,3, 4,2,2, 1,2,3,4,5,5,5}; // 8. 粉刷匠

const int* SONGS[8] = {S1, S2, S3, S4, S5, S6, S7, S8};
const int SONG_LENGTHS[8] = {
  sizeof(S1)/sizeof(int), sizeof(S2)/sizeof(int), sizeof(S3)/sizeof(int), sizeof(S4)/sizeof(int),
  sizeof(S5)/sizeof(int), sizeof(S6)/sizeof(int), sizeof(S7)/sizeof(int), sizeof(S8)/sizeof(int)
};

int currentSong = 0;
int meledyPtr = 0;

// ─── 硬件设置 ──────────────────────────────────────────────────────────
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

// 全局按键状态（防止状态切换时误触）
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

// ─── I2S 音频后台任务 ──────────────────────────────────────────────────
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

// ─── 核心功能函数 ────────────────────────────────────────────────────────
void playTone(float freq, int dur) {
  targetFreq = freq; soundStopTime = millis() + dur; 
}

void playNextSongNote() {
  int noteNum = SONGS[currentSong][meledyPtr];
  playTone(KEY_FREQS[noteNum - 1], 120); 
  meledyPtr = (meledyPtr + 1) % SONG_LENGTHS[currentSong];
}

void updateScoreDisplay() {
  if (score < 0) score = 0; 
  display.showNumberDec(score, false);
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

// ─── 统一读取按键 ────────────────────────────────────────────────────────
void readInputs() {
  static bool lastState[8] = {false};
  for(int i=0; i<8; i++) {
    bool current = (touchRead(TOUCH_PINS[i]) < TOUCH_THRESHOLD);
    keyJustPressed[i] = (current && !lastState[i]); // 只有按下的瞬间才为 true
    lastState[i] = current;
  }
}

// ─── 游戏菜单 (选歌界面) ─────────────────────────────────────────────────
void updateMenu() {
  allOff();
  // 底部蓝青色呼吸灯，提示玩家按键选歌
  int pulse = (sin(millis() / 200.0) * 100) + 100;
  for(int i=0; i<8; i++) {
    strip.setPixelColor(ledMatrix[3][i], strip.Color(0, pulse / 2, pulse));
  }
  strip.show();

  // 显示 "----" 代表等待选歌
  uint8_t data[] = { 0x40, 0x40, 0x40, 0x40 }; 
  display.setSegments(data);

  for(int c=0; c<8; c++) {
    if (keyJustPressed[c]) {
      currentSong = c;          // 锁定选择的歌曲
      meledyPtr = 0;            // 从第一句开始弹
      score = 0;                // 分数清零
      updateScoreDisplay();
      initFourNotes();          // 重新生成掉落方块
      
      gameState = STATE_PLAYING;// 切换到游戏中状态
      lastMove = millis();
      
      playTone(KEY_FREQS[SONGS[c][0]-1], 150); // 弹一下第一颗音符
      delay(500); // 停顿半秒，给玩家准备时间
      return;
    }
  }
}

// ─── 胜利判定及动画 ──────────────────────────────────────────────────────
void checkWinCondition() {
  if (score >= 50) {
    gameState = STATE_WIN;
    winStartTime = millis();
    
    // 播放胜利通关音效 (登登登-灯!)
    playTone(KEY_FREQS[0], 100); delay(100);
    playTone(KEY_FREQS[2], 100); delay(100);
    playTone(KEY_FREQS[4], 100); delay(100);
    playTone(KEY_FREQS[7], 400); 
  }
}

void updateWin() {
  display.showNumberDec(100, false); // 固定显示100分
  
  // 彩虹炫光流水灯特效
  unsigned long now = millis();
  long firstPixelHue = (now * 50) % 65536; 
  for(int i=0; i<strip.numPixels(); i++) {
    int pixelHue = firstPixelHue + (i * 65536L / strip.numPixels());
    strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(pixelHue)));
  }
  strip.show();

  // 4秒后自动回到菜单页面
  if (now - winStartTime > 4000) { 
    gameState = STATE_MENU;
  }
}

// ─── 游戏主逻辑 (同之前) ────────────────────────────────────────────────
void updateNotes() {
  int cnt = 0;
  for (int i=0; i<MAX_NOTES; i++) {
    if (!notes[i].active) continue;
    notes[i].pos += speed;
    if (notes[i].hitState == 0 && notes[i].pos > HIT_END) {
      notes[i].hitState = 2; wrongPressTime[notes[i].col] = millis();
      score -= 1; updateScoreDisplay();
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
      playNextSongNote();
      keyJustPressed[target] = false; 

      render();   // 强制立刻把这一帧的绿灯刷出来
      delay(80);  // 顿帧效果
      
      checkWinCondition(); // 检查是否达到100分
      if (gameState != STATE_PLAYING) return; // 如果赢了，立刻停止后续判定

    } else {
      for (int c=0; c<8; c++) {
        if (keyJustPressed[c]) {
          notes[i].hitState = 2; wrongPressTime[c] = millis();
          score -= 1; updateScoreDisplay();
          keyJustPressed[c] = false;     
        }
      }
    }
  }
  // 空挥判定
  for (int c=0; c<8; c++) {
    if (keyJustPressed[c]) {
      wrongPressTime[c] = millis();
      score -= 1; updateScoreDisplay();
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
  
  gameState = STATE_MENU; // 开机直接进入菜单状态
}

void loop() {
  readInputs(); // 【极其重要】全局统一扫描按键状态

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
}
  
 /*

  #include <Arduino.h>

const int DAC_PIN = 25;
const int SAMPLE_RATE = 8000;
const int TABLE_SIZE = 256;

uint8_t sineTable[TABLE_SIZE];

hw_timer_t *timer = NULL;
volatile uint16_t phase16 = 0;
volatile uint16_t phaseInc16 = 0;
volatile uint8_t amplitude = 80; // 0–127

void IRAM_ATTR onTimer() {
  uint8_t idx = phase16 >> 8;
  uint8_t sample = 127 + (int8_t)(((int16_t)sineTable[idx] - 127) * amplitude / 127);
  dacWrite(DAC_PIN, sample);
  phase16 += phaseInc16;
}

// C2 major scale (two octaves lower)
const float notes[] = {65.41, 73.42, 82.41, 87.31, 98.00, 110.00, 123.47, 130.81};
const char* names[] = {"Do", "Re", "Mi", "Fa", "Sol", "La", "Ti", "Do"};
const int NOTE_DURATION_MS = 500;
const int NOTE_GAP_MS = 50;
const int numNotes = sizeof(notes) / sizeof(notes[0]);

void playNote(float freq) {
  phaseInc16 = (uint16_t)(freq * TABLE_SIZE * 256.0f / SAMPLE_RATE);
}

void silence() {
  phaseInc16 = 0;
  dacWrite(DAC_PIN, 127); // rest at midpoint to avoid click
}

void setup() {
  Serial.begin(115200);

  for (int i = 0; i < TABLE_SIZE; i++) {
    sineTable[i] = (uint8_t)(127 + 127 * sinf(2.0f * PI * i / TABLE_SIZE));
  }

  timer = timerBegin(0, 80, true);
  timerAttachInterrupt(timer, &onTimer, true);
  timerAlarmWrite(timer, 1000000 / SAMPLE_RATE, true);
  timerAlarmEnable(timer);
}

void loop() {
  for (int i = 0; i < numNotes; i++) {
    Serial.println(names[i]);
    playNote(notes[i]);
    delay(NOTE_DURATION_MS);
    silence();
    delay(NOTE_GAP_MS);
  }

  delay(300);
}
  */
  