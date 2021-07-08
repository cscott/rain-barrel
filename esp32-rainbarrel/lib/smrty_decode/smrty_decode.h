#ifndef SMRTY_DECODE_H
#define SMRTY_DECODE_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define CYCLE_FREQ 19200
// At COUNTS_PER_CYCLE=256, we roll over our 32-bit counter every 873s
#define COUNTS_PER_CYCLE 256
#define COUNTS_PER_HALFCYCLE (COUNTS_PER_CYCLE/2)

struct smrty_msg {
    uint8_t addr;
    uint8_t cmd;
    uint8_t tx_data1;
    uint8_t tx_data2;
    uint8_t rx_data1;
    uint8_t rx_data2;
    uint8_t status;
    uint8_t checksum;
} __attribute__((packed));

// Process a single input transition (either L->H or H->L) at cycle_count
// Returns a pointer to a (statically allocated) smrty_msg if this completes
// decode of a complete message, or else returns NULL for "more input needed".
struct smrty_msg *process_transition(uint32_t cycle_count);

#ifdef __cplusplus
}
#endif
#endif /* SMRTY_DECODE_H */
