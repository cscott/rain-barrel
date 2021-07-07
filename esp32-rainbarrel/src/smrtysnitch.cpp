#ifdef SMRTYSNITCH
// A protocol decoder for the SMRT-Y soil moisture meter
#include "Arduino.h"
#include <Wire.h>
#include <AsyncDelay.h>
#include "smrtysnitch.h"

// It appears the Arduino port to the Raspberry Pi Pico that we are using
// hardwires I2C to GPIO 6 (SDA) and 7 (SCL) in
// in https://github.com/arduino/ArduinoCore-mbed/blob/master/variants/RASPBERRY_PI_PICO/pins_arduino.h
#define GPIO_SDA0 6
#define GPIO_SCL0 7

int64_t bogus_count = 1;
AsyncDelay blinkDelay = AsyncDelay(500, AsyncDelay::MILLIS);
bool wasOn = false;

void requestEvent();

// the setup routine runs once when you press reset:
void setup() {
  // initialize the digital pin as an output.
  pinMode(LED_BUILTIN, OUTPUT);
  Wire.begin(SNITCH_I2C_ADDR); // join i2c bus with address
  Wire.onRequest(requestEvent); // register event
  blinkDelay.expire();
}

void loop() {
    if (blinkDelay.isExpired()) {
        blinkDelay.repeat();
        if (wasOn) {
            digitalWrite(LED_BUILTIN, LOW);
        } else {
            digitalWrite(LED_BUILTIN, HIGH);
        }
        wasOn = !wasOn;
        bogus_count++;
    }
}

// function that executes whenever data is requested by master
// this function is registered as an event, see setup()
void requestEvent() {
    // 8 bytes, LSB first
    Wire.write((uint8_t*)&bogus_count, sizeof(bogus_count));
}

#endif /* SMRTYSNITCH */
