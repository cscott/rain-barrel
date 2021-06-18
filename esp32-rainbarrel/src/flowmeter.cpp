#ifdef FLOWMETER
#include "Arduino.h"
#include <Wire.h>
#include "flowmeter.h"

#define LED_PIN 1
#define TURBINE_PIN 5
#define DEBOUNCE 10

void requestEvent();

int lastLevel;
int debounceCount;
int64_t pulse_count;

void setup() {
    pinMode(LED_PIN, OUTPUT);
    pinMode(TURBINE_PIN, INPUT);
    Wire.begin(FLOWMETER_I2C_ADDR); // join i2c bus with address
    Wire.onRequest(requestEvent);   // register event
    lastLevel = digitalRead(TURBINE_PIN);
    digitalWrite(LED_PIN, lastLevel);
    debounceCount = 0;
    pulse_count = 0;
}

void loop() {
    int level = digitalRead(TURBINE_PIN);
    if (level == lastLevel) {
        debounceCount = 0;
    } else if ((++debounceCount) > DEBOUNCE) {
        debounceCount = 0;
        lastLevel = level;
        pulse_count++;
        digitalWrite(LED_PIN, level);
    }
}

// function that executes whenever data is requested by master
// this function is registered as an event, see setup()
void requestEvent() {
    // 8 bytes, LSB first
    Wire.write((uint8_t*)&pulse_count, sizeof(pulse_count));
}

#endif /* FLOWMETER */
