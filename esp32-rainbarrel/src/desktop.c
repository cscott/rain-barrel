#ifdef NATIVE_BUILD
#include <stdio.h>
#include <stdlib.h>
#include "smrty_decode.h"

int main(int argc, char **argv) {
    // open the file named on the command line
    if (argc != 2) {
        return 1;
    }
    FILE *in = fopen(argv[1], "r");
    char buf[256], *s;
    while (fgets(buf, sizeof(buf), in) != NULL) {
        long long l = atoll(buf);
        uint32_t tick = (uint32_t) l; // deliberately truncate
        struct smrty_msg *msg = process_transition(tick);
        if (msg != NULL) {
            uint8_t checksum = 0;
            for (int i=0; i<7; i++) {
                checksum += ((uint8_t*)msg)[i];
            }
            printf("%02X %02X %02X %02X | %02X %02X %02X | %02X%s\n",
                   msg->addr, msg->cmd, msg->tx_data1, msg->tx_data2,
                   msg->rx_data1, msg->rx_data2, msg->status,
                   msg->checksum, (checksum==msg->checksum)?"":" [bad]");
        }
    }
    fclose(in);
    return 0;
}

#endif /* NATIVE_BUILD */
