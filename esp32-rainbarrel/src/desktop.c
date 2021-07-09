#ifdef NATIVE_BUILD
#include <stdio.h>
#include <stdlib.h>
#include "smrty_decode.h"

void maybe_print_msg(struct smrty_msg *msg) {
    if (msg==NULL) {
        return;
    }
    uint8_t checksum = 0;
    for (int i=0; i<7; i++) {
        checksum += ((uint8_t*)msg)[i];
    }
    printf("%02X %02X %02X %02X | %02X %02X %02X | %02X%s\n",
           msg->addr, msg->cmd, msg->tx_data1, msg->tx_data2,
           msg->rx_data1, msg->rx_data2, msg->status,
           msg->checksum, (checksum==msg->checksum)?"":" [bad]");
}

int main(int argc, char **argv) {
    // open the file named on the command line
    if (argc != 2) {
        return 1;
    }
    FILE *in = fopen(argv[1], "r");
    char buf[256], *s;
    uint32_t last_tick;
    struct smrty_msg *msg;
    while (fgets(buf, sizeof(buf), in) != NULL) {
        long long l = atoll(buf);
        last_tick = (uint32_t) l; // deliberately truncate
        msg = process_transition(last_tick);
        maybe_print_msg(msg);
    }
    fclose(in);
    // Feed in one last (watchdog) tick.
    last_tick += 0x0800000;
    msg = process_transition(last_tick);
    maybe_print_msg(msg);
    return 0;
}

#endif /* NATIVE_BUILD */
