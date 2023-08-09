#ifdef SMRTYSNITCH
// A protocol decoder for the SMRT-Y soil moisture meter
#include "Arduino.h"
#include <Wire.h>
#include "smrtysnitch.h"
#include "smrty_decode.h"
#include "pico/binary_info.h"

#define EARLEPHILHOWER
//#define DEBUGGING
//#define USE_MUTEX
//#define LED_SHOW_WATCHDOG

#ifndef SNITCH_OFFSET
# error SNITCH_OFFSET must be defined (is this snitch #1 or #2?)
#endif

// safety first
#ifdef EARLEPHILHOWER
# define WireX Wire1
# include <FreeRTOS.h>
# include <semphr.h>
SemaphoreHandle_t msg_lock_h = NULL;
StaticSemaphore_t msg_lock_buffer;
# define MUTEX_INIT() do {                                           \
        msg_lock_h = xSemaphoreCreateMutexStatic( &msg_lock_buffer); \
    } while(false)
# define MUTEX_TAKE()                                 \
    while (xSemaphoreTake( msg_lock_h, 1) != pdTRUE) { \
        /* busy loop */;                                \
    }
# define MUTEX_TRY_TAKE() \
    (xSemaphoreTake(msg_lock_h, 0) == pdTRUE)
# define MUTEX_RELEASE() do {                   \
        xSemaphoreGive(msg_lock_h);             \
    } while(false)
#else
# define WireX Wire
# include <mutex.h>
auto_init_mutex(msg_lock);
# define MUTEX_INIT() do { } while(false)
# define MUTEX_TAKE() mutex_enter_blocking(&msg_lock)
# define MUTEX_TRY_TAKE() mutex_try_enter(&msg_lock, NULL)
# define MUTEX_RELEASE() mutex_exit(&msg_lock)
#endif

// Black magic
#include <hardware/clocks.h>
#include <hardware/pio.h>
#include "smrty.pio.h"

// It appears the Arduino port to the Raspberry Pi Pico that we are using
// hardwires I2C to GPIO 6 (SDA) and 7 (SCL) in
// in https://github.com/arduino/ArduinoCore-mbed/blob/master/variants/RASPBERRY_PI_PICO/pins_arduino.h
#define GPIO_SDA0 6
#define GPIO_SCL0 7
// BUT IN https://github.com/arduino/ArduinoCore-mbed/commit/4ccfcda17b1e65337cff8cb37af71008d67a0106
// they changed this to 4 and 5!  So we'll wire to both pins, and set the
// "old" pins to passive/unused inputs.
#define GPIO_SDA1 4
#define GPIO_SCL1 5
// (In the end we switched to the earl philhower arduino port, which allows us
// to customize the I2C pin assignments.  But the rewiring had already been
// done.)

#define SMRTY_GPIO_PIN 2

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

void requestEvent();
void receiveEvent(int numBytes);

#define INPUT_NO_PULL(pin) do {                 \
        pinMode(pin, INPUT);                    \
        gpio_set_dir(pin, false);               \
        gpio_disable_pulls(pin);                \
        gpio_set_input_enabled(pin, true);      \
    } while (false)


// the setup routine runs once when you press reset:
void setup() {
    bi_decl(bi_program_name("SMRTY snitch"));
    bi_decl(bi_program_description("I2C slave that reports decoding SMRTY information"));
    bi_decl(bi_1pin_with_name(SMRTY_GPIO_PIN, "SMRTY transition input"));
    bi_decl(bi_2pins_with_func(GPIO_SDA0, GPIO_SCL0, GPIO_FUNC_I2C));
    bi_decl(bi_1pin_with_name(LED_BUILTIN, "Debugging LED"));
#ifdef USE_MUTEX
  MUTEX_INIT();
#endif
  // initialize the digital pin as an output.
  pinMode(LED_BUILTIN, OUTPUT);
  INPUT_NO_PULL(SMRTY_GPIO_PIN);
#if 1 // def DEBUGGING
  gpio_pull_up(SMRTY_GPIO_PIN);
#endif
  for (int i=0; i<SNITCH_BUFFER_SIZE; i++) {
      i2c_buffer[i].seqno = 0xFF;
  }

  // Initialize all possible I2C pins as inputs
  INPUT_NO_PULL(GPIO_SDA0);
  INPUT_NO_PULL(GPIO_SCL0);
  INPUT_NO_PULL(GPIO_SDA1);
  INPUT_NO_PULL(GPIO_SCL1);
  // This will be *either* GPIO_Sxx0 (old ArduinoCore-mbed) *or* GPIO_Sxx1
  // (latest ArduinoCore-mbed). Sigh. We'll run wires to both.
#ifdef EARLEPHILHOWER
  WireX.setSDA(GPIO_SDA0); // earlephilhower port only
  WireX.setSCL(GPIO_SCL0); // earlephilhower port only
#endif
  WireX.begin(SNITCH_I2C_ADDR_BASE + SNITCH_OFFSET); // join i2c bus with address
  WireX.onReceive(receiveEvent); // register event (bytes from master)
  WireX.onRequest(requestEvent); // register event (bytes to master)
#ifndef DEBUGGING

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
#endif
  // and tell the world we're alive!
  digitalWrite(LED_BUILTIN, HIGH);
  wasOn = true;
}

void loop() {
    // the requestEvent is apparently interrupt-driven, because this works
    // even if we use a blocking read here.
#ifdef DEBUGGING
    //longcount++;
#else
    uint32_t cycle;
    if (!pio_sm_is_rx_fifo_empty(pio, smrty_sm)) {
        cycle = pio_sm_get_blocking(pio, smrty_sm);
    } else if (!pio_sm_is_rx_fifo_empty(pio, watchdog_sm)) {
        cycle = pio_sm_get_blocking(pio, watchdog_sm);
#ifdef LED_SHOW_WATCHDOG
        // blink the LED w/ the watchdog as well
        digitalWrite(LED_BUILTIN, wasOn ? LOW : HIGH);
        wasOn = !wasOn;
#endif
    } else {
#ifdef DEBUGGING_PIO
        //longcount++;
#endif
        return; // nothing to do!
    }
#ifdef DEBUGGING_PIO
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
    // Ignore all zeros, generated by the newer SMRT-Y snitch decoder hardware
    bool all_zeros = true;
    for (int i=0; i<8; i++) {
      if (((uint8_t*)msg)[i] != 0) {
        all_zeros = false;
        break;
      }
    }
    if (all_zeros) {
      return;
    }

#ifdef USE_MUTEX
    MUTEX_TAKE();
#else
    noInterrupts();
#endif
    memmove(&(i2c_buffer[1]), &(i2c_buffer[0]),
            sizeof(i2c_buffer[0])*(SNITCH_BUFFER_SIZE-1));
    i2c_buffer[0].seqno = seqno++;
    memmove(&(i2c_buffer[0].msg), msg, sizeof(*msg));
    // roll over after 7 bits so that we can use 0xFF to indicate 'not here'
    if (seqno >= 0x80) { seqno = 0; }
#ifdef USE_MUTEX
    MUTEX_RELEASE();
#else
    interrupts();
#endif
#endif
}

static uint8_t register_requested = 0;

void receiveEvent(int numBytes) {
    if (numBytes > 0) {
        register_requested = WireX.read();
        numBytes--;
    }
    for (; numBytes > 0; numBytes--) {
        WireX.read();
    }
#if defined(DEBUGGING) || !defined(LED_SHOW_WATCHDOG)
    wasOn = !wasOn;
    digitalWrite(LED_BUILTIN, wasOn);
#endif
#if !defined(DEBUGGING)
    // If the register requested is "high enough", toggle a GPIO (bonus feature)
    if (register_requested >= 0xC0) {
        uint8_t level = (register_requested & 1);
        uint8_t pin = (register_requested >> 1) & 0x1F;
        if (pin != SMRTY_GPIO_PIN &&
            pin != GPIO_SDA0 && pin != GPIO_SCL0 &&
            pin != GPIO_SDA1 && pin != GPIO_SCL1) {
            // Sanity-check: don't let the I2C interface mess up our pin config
            pinMode(pin, OUTPUT);
            digitalWrite(pin, level);
        }
    }
#endif
}

// function that executes whenever data is requested by master
// this function is registered as an event, see setup()
void requestEvent() {
    // Return the register number; this allows sender to see if we've
    // processed the register write yet.
#ifdef DEBUGGING
    WireX.write(register_requested);
    for (int i=0; i<5; i++) {
        WireX.write(0xFF);
    }
    WireX.write((uint8_t*)&longcount, 4);
    longcount++;
#else
#ifdef USE_MUTEX
    if (MUTEX_TRY_TAKE()) {
#endif
        // library has a 256 byte buffer, so this should be safe.
        // (ie, it shouldn't block)
        // philhower port writes directly to TX FIFO, but that's 16 bytes long
        // too -- a little tighter, but should still be safe.
        uint8_t idx = (register_requested % SNITCH_BUFFER_SIZE);
        WireX.write(register_requested);
        WireX.write(&(i2c_buffer[idx].seqno), 1);
        WireX.write((uint8_t*)&(i2c_buffer[idx].msg), 8);
#ifdef USE_MUTEX
        MUTEX_RELEASE();
    } else {
        // Tell the caller to wait
        for (int i=0; i<10; i++) {
            // This won't match requested register # so master will retry.
            WireX.write(0xFF);
        }
    }
#endif
#endif
}

#endif /* SMRTYSNITCH */
