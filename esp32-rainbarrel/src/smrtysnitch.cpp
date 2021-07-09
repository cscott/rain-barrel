#ifdef SMRTYSNITCH
// A protocol decoder for the SMRT-Y soil moisture meter
#include "Arduino.h"
#include <Wire.h>
#include "smrtysnitch.h"
#include "smrty_decode.h"

// Black magic
#include <hardware/clocks.h>
#include <hardware/pio.h>
#include <mutex.h>
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
uint smrty_sm, watchdog_sm;
uint8_t seqno = 0;
uint8_t msgno = 0;

// i2c message buffer
struct smrty_msg i2c_buffer[3];

// safety first
auto_init_mutex(msg_lock);

void requestEvent();

// the setup routine runs once when you press reset:
void setup() {
  // initialize the digital pin as an output.
  pinMode(LED_BUILTIN, OUTPUT);
  memset(i2c_buffer, 0, sizeof(i2c_buffer));

  Wire.begin(SNITCH_I2C_ADDR); // join i2c bus with address
  Wire.onRequest(requestEvent); // register event

  pio = pio0; // ..or pio1
  uint offset = pio_add_program(pio, &smrty_program);
  smrty_sm = pio_claim_unused_sm(pio, true);
  smrty_program_init(pio, smrty_sm, offset,
                     SMRTY_GPIO_PIN, CYCLE_FREQ*COUNTS_PER_CYCLE);

  offset = pio_add_program(pio, &watchdog_program);
  watchdog_sm = pio_claim_unused_sm(pio, true);
  watchdog_program_init(pio, watchdog_sm, offset,
                        SMRTY_GPIO_PIN, CYCLE_FREQ*COUNTS_PER_CYCLE);

  // Now enable both programs in sync.
  pio_enable_sm_mask_in_sync(pio, (1<<smrty_sm)|(1<<watchdog_sm));
}

void loop() {
    // the requestEvent is apparently interrupt-driven, because this works
    // even if we use a blocking read here.
    uint32_t cycle;
    if (!pio_sm_is_rx_fifo_empty(pio, smrty_sm)) {
        cycle = pio_sm_get_blocking(pio, smrty_sm);
    } else if (!pio_sm_is_rx_fifo_empty(pio, watchdog_sm)) {
        cycle = pio_sm_get_blocking(pio, watchdog_sm);
        // blink the LED w/ the watchdog as well
        digitalWrite(LED_BUILTIN, wasOn ? LOW : HIGH);
        wasOn = !wasOn;
    } else {
        return; // nothing to do!
    }
    struct smrty_msg *msg = process_transition(~cycle);
    if (msg == NULL) {
        return; // no message yet
    }
    // OK! Set up something for I2C to read!
    if (msg->cmd == 0x08) {
        // Ignore the "wake up" command but use it to reset the msgno
        msgno = 0;
        while ((seqno&3)!=0) { seqno++; }
        return;
    }
    mutex_enter_blocking(&msg_lock);
    memmove(&(i2c_buffer[msgno]), msg, sizeof(*msg));
    msgno++; seqno++;
    if (msgno==3) {
        seqno++; // use lower 2 bits to indicate that read is complete
        msgno = 0;
    }
    mutex_exit(&msg_lock);
}

// function that executes whenever data is requested by master
// this function is registered as an event, see setup()
void requestEvent() {
    if (mutex_try_enter(&msg_lock, NULL)) {
        // library has a 256 byte buffer, so this should be safe.
        // (ie, it shouldn't block)
        Wire.write(&seqno, sizeof(seqno));
        Wire.write((uint8_t*)i2c_buffer, sizeof(i2c_buffer));
        mutex_exit(&msg_lock);
    } else {
        // Tell the caller to wait
        uint8_t not_ready = 0xFF;
        Wire.write(&not_ready, sizeof(not_ready));
    }
}

#endif /* SMRTYSNITCH */
