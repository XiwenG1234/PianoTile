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