// Implementation of smarty decode
#include <stdlib.h>
#include <string.h>
#include "smrty_decode.h"

//#define DEBUGGING
#ifdef DEBUGGING
#include <stdio.h>
// Saleae Logic export offsets the time for some reason.  This correction
// factor was manually measured by comparing times in the export with
// times displayed in the UX. Ugh.
#define FUDGE_FACTOR (85.365*.000001) /* seconds */
#define SECONDS(x) ((x/(double)(19200*COUNTS_PER_CYCLE))+FUDGE_FACTOR)
#endif

struct smrty_msg msg;

enum decode_state {
    INITIAL,
    LOOKING_FOR_BEEP,
    LOOKING_FOR_PHASE_REVERSAL,
    LOOKING_FOR_BIT,
};

struct smrty_msg *process_transition(uint32_t cycle_count) {
    static enum decode_state state = INITIAL;
    static uint32_t last_transition = 0;
    uint32_t interval = (cycle_count - last_transition);

// GOOD_PROLOGUE should be odd so we're measuring pos-pos or neg-neg
#define GOOD_PROLOGUE 7 // look for 3 1/2 cycles
    static uint8_t num_chirps = 0;
    static uint32_t chirp_centers[GOOD_PROLOGUE];
    static uint32_t next_chip;
    static uint32_t chip_pattern;
    static uint8_t chip_level;
    static uint32_t start_byte0;
    static uint8_t bit_count;
    static uint8_t *resultPtr;
#ifdef DEBUGGING
    //printf("At %lld state %d interval %lld\n", (long long) cycle_count, state, (long long)interval);
#endif

    switch(state) {
    case INITIAL:
        state = LOOKING_FOR_BEEP;
        num_chirps = 0;
        break;
    case LOOKING_FOR_BEEP:
        // Look for a good # of transitions in a row where the period is 19.2kHz
        // +/- 10%.  There's going to be noise around the edges, ignore that.
        if (interval < (COUNTS_PER_HALFCYCLE*9/10) ||
            interval > (COUNTS_PER_HALFCYCLE*11/10)) {
            break; // not a beep
        }
        chirp_centers[num_chirps++] = cycle_count - (interval/2);
        if (num_chirps!=GOOD_PROLOGUE) {
            // We haven't seen enough chirps yet
            break;
        }
        // Is the total duration what we expect?
        interval = chirp_centers[GOOD_PROLOGUE-1] - chirp_centers[0];
        if (interval < (COUNTS_PER_HALFCYCLE*(GOOD_PROLOGUE-1)*9/10) ||
            interval > (COUNTS_PER_HALFCYCLE*(GOOD_PROLOGUE-1)*11/10)) {
            // nope, keep looking
            memmove(&(chirp_centers[0]), &(chirp_centers[1]), sizeof(chirp_centers) - sizeof(*chirp_centers));
            num_chirps--;
            break;
        }
        // oh, this is good!  Let's look for the phase reversal!
        state = LOOKING_FOR_PHASE_REVERSAL;
        next_chip = chirp_centers[GOOD_PROLOGUE-1] + COUNTS_PER_HALFCYCLE;
        chip_pattern = 0; // bit 0 is arbitrary, could be 1
#ifdef DEBUGGING
        printf("Found chirp, last interval (%lf - %lf)\n",
               SECONDS(last_transition),
               SECONDS(cycle_count));
        printf("Chirp centers %lf %lf %lf %lf\n",
               SECONDS(chirp_centers[GOOD_PROLOGUE-4]),
               SECONDS(chirp_centers[GOOD_PROLOGUE-3]),
               SECONDS(chirp_centers[GOOD_PROLOGUE-2]),
               SECONDS(chirp_centers[GOOD_PROLOGUE-1]));
        printf("Expect next chip at %lf\n", SECONDS(next_chip));
#endif
        break;
    case LOOKING_FOR_PHASE_REVERSAL:
        interval = cycle_count - chirp_centers[GOOD_PROLOGUE-1];
        if (interval > 20*COUNTS_PER_CYCLE) {
            // give up, return to start state
            state = INITIAL;
            break;
        }
        while ((cycle_count - (int64_t)next_chip) >= 0) { // careful w/ rollover
            // ok, this is the chip, shift it
            chip_pattern = (chip_pattern << 1) | (chip_pattern & 1);
            if ((chip_pattern & 0x1CE) == 0x14A ||
                ((~chip_pattern) & 0x1CE) == 0x14A) {
                // found phase reversal!  The start of each burst of 10 cycles
                // starts exactly 3.25 cycle times later
                start_byte0 = next_chip + (13*COUNTS_PER_CYCLE/4);
                //start_byte0 = cycle_count + 3*COUNTS_PER_CYCLE;
                next_chip = start_byte0;
                bit_count = 0;
                num_chirps = 0;
                resultPtr = (uint8_t*)&msg;
                state = LOOKING_FOR_BIT;
#ifdef DEBUGGING
                printf("Found phase reversal; next_chip at %lf cycle_count %lf start at %lf\n", SECONDS(next_chip), SECONDS(cycle_count), SECONDS(start_byte0));
#endif
                return NULL;
            }
            next_chip += COUNTS_PER_HALFCYCLE;
        }
        chip_pattern = chip_pattern ^ 1;
        break;
    case LOOKING_FOR_BIT:
    restart_bit:
        // fixme: could get stuck if last transition is to low?
        // we want to ensure we get a transition every second or so
        // to flush this queue after the last bit.
#ifdef DEBUGGING
        //printf("Looking for bit at %lld\n", (long long) cycle_count);
#endif
        // careful about rollover!
        if ((cycle_count - (int64_t)next_chip) > 10*COUNTS_PER_CYCLE) {
            // this bit is done.
#ifdef DEBUGGING
            printf("Bit #%d (%lf) chirps=%d\n", bit_count, SECONDS(next_chip), num_chirps);
#endif
            *resultPtr = ((*resultPtr) >> 1) | ((num_chirps < 12) ? 0 : 0x80);
            num_chirps = 0;
            bit_count++;
            if (bit_count == 8*8) { // done!
                state = INITIAL;
                return &msg;
            }
            if ((bit_count&7) == 0) {
                resultPtr++;
            }
            // compute next bit start time
            next_chip = start_byte0 +
                (bit_count&7)*(10*COUNTS_PER_CYCLE) +
                (bit_count/8)*(320*COUNTS_PER_CYCLE) /* 60Hz (19200/320) */;
            goto restart_bit;
        }
        if ((next_chip - (int64_t)last_transition) > 0) {
            break; // ignore everything until next_chip
        }
#ifdef DEBUGGING
        //printf("Interval %lld at %lld\n", (long long) interval, cycle_count);
#endif
        // is this interval a chirp?
        if (interval < (COUNTS_PER_HALFCYCLE*9/10) ||
            interval > (COUNTS_PER_HALFCYCLE*11/10)) {
            break; // not a chirp
        }
        // this is a chirp, whee!
        num_chirps++;
        break;
    }
    last_transition = cycle_count;
    return NULL;
}
