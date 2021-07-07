#ifdef SMRTYSNITCH
// A protocol decoder for the SMRT-Y soil moisture meter
#include "Arduino.h"
#include <Wire.h>
#include "smrtysnitch.h"

// Black magic
#include <hardware/clocks.h>
#include <hardware/pio.h>
#include "smrty.pio.h"

// It appears the Arduino port to the Raspberry Pi Pico that we are using
// hardwires I2C to GPIO 6 (SDA) and 7 (SCL) in
// in https://github.com/arduino/ArduinoCore-mbed/blob/master/variants/RASPBERRY_PI_PICO/pins_arduino.h
#define GPIO_SDA0 6
#define GPIO_SCL0 7

#define SMRTY_GPIO_PIN 2

int64_t bogus_count = 42;
bool wasOn = false;
PIO pio;
uint sm;

void requestEvent();

// the setup routine runs once when you press reset:
void setup() {
  // initialize the digital pin as an output.
  pinMode(LED_BUILTIN, OUTPUT);
  Wire.begin(SNITCH_I2C_ADDR); // join i2c bus with address
  Wire.onRequest(requestEvent); // register event

  pio = pio0; // ..or pio1
  uint offset = pio_add_program(pio, &smrty_program);
  sm = pio_claim_unused_sm(pio, true);
  smrty_program_init(pio, sm, offset, SMRTY_GPIO_PIN, 19200*256);
}

void loop() {
    // the requestEvent is apparently interrupt-driven, because this works
    // even if we use a blocking read here.
    if (true || !pio_sm_is_rx_fifo_empty(pio, sm)) {
        bogus_count = -pio_sm_get_blocking(pio, sm);
    }
}

// function that executes whenever data is requested by master
// this function is registered as an event, see setup()
void requestEvent() {
    // 8 bytes, LSB first
    Wire.write((uint8_t*)&bogus_count, sizeof(bogus_count));
}

#endif /* SMRTYSNITCH */
