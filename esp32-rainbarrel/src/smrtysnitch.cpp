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

//#define DEBUGGING

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

// i2c message buffer
#define SNITCH_BUFFER_SIZE 8
struct buffer {
    uint8_t seqno;
    struct smrty_msg msg;
} i2c_buffer[SNITCH_BUFFER_SIZE];

#ifdef DEBUGGING
static uint32_t longcount = 0;
#endif

// safety first
auto_init_mutex(msg_lock);

void requestEvent();
void receiveEvent(int numBytes);

// the setup routine runs once when you press reset:
void setup() {
  // initialize the digital pin as an output.
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(SMRTY_GPIO_PIN, INPUT);
  gpio_set_dir(SMRTY_GPIO_PIN, false);
  gpio_disable_pulls(SMRTY_GPIO_PIN);
  gpio_set_input_enabled(SMRTY_GPIO_PIN, true);
#if 1 // def DEBUGGING
  gpio_pull_up(SMRTY_GPIO_PIN);
#endif
  for (int i=0; i<SNITCH_BUFFER_SIZE; i++) {
      i2c_buffer[i].seqno = 0xFF;
  }

  Wire.begin(SNITCH_I2C_ADDR); // join i2c bus with address
  Wire.onReceive(receiveEvent); // register event (bytes from master)
  Wire.onRequest(requestEvent); // register event (bytes to master)

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
  // and tell the world we're alive!
  digitalWrite(LED_BUILTIN, HIGH);
  wasOn = true;
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
#ifdef DEBUGGING
        //longcount++;
#endif
        return; // nothing to do!
    }
#ifdef DEBUGGING
    longcount = cycle;
#endif
    struct smrty_msg *msg = process_transition(~cycle);
    if (msg == NULL) {
        return; // no message yet
    }
    // OK! Set up something for I2C to read!
    if (msg->cmd == 0x08) {
        // Ignore the "wake up" command
        return;
    }
    mutex_enter_blocking(&msg_lock);
    memmove(&(i2c_buffer[1]), &(i2c_buffer[0]),
            sizeof(i2c_buffer[0])*(SNITCH_BUFFER_SIZE-1));
    i2c_buffer[0].seqno = seqno++;
    memmove(&(i2c_buffer[0].msg), msg, sizeof(*msg));
    // roll over after 7 bits so that we can use 0xFF to indicate 'not here'
    if (seqno >= 0x80) { seqno = 0; }
    mutex_exit(&msg_lock);
}

static uint8_t register_requested = 0;

void receiveEvent(int numBytes) {
    if (numBytes > 0) {
        register_requested = Wire.read();
        numBytes--;
    }
    for (; numBytes > 0; numBytes--) {
        Wire.read();
    }
}

// function that executes whenever data is requested by master
// this function is registered as an event, see setup()
void requestEvent() {
    // Return the register number; this allows sender to see if we've
    // processed the register write yet.
#ifdef DEBUGGING
    Wire.write(register_requested);
    for (int i=0; i<5; i++) {
        Wire.write(0xFF);
    }
    Wire.write((uint8_t*)&longcount, 4);
#else
    if (mutex_try_enter(&msg_lock, NULL)) {
        // library has a 256 byte buffer, so this should be safe.
        // (ie, it shouldn't block)
        uint8_t idx = (register_requested % SNITCH_BUFFER_SIZE);
        Wire.write(register_requested);
        Wire.write(&(i2c_buffer[idx].seqno), 1);
        Wire.write((uint8_t*)&(i2c_buffer[idx].msg), 8);
        mutex_exit(&msg_lock);
    } else {
        // Tell the caller to wait
        for (int i=0; i<10; i++) {
            // This won't match requested register # so master will retry.
            Wire.write(0xFF);
        }
    }
#endif
}

#endif /* SMRTYSNITCH */
