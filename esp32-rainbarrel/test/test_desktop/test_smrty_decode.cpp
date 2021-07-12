#include <unity.h>
#include "smrty_decode.h"

#define WATCHDOG_VALUE 0x08000000

uint32_t data_20210706[] = {
#include "sample-20210706.h"
WATCHDOG_VALUE
};

uint32_t data_20210708a[] = {
#include "sample-20210708a.h"
WATCHDOG_VALUE
};

uint32_t data_20210708b[] = {
#include "sample-20210708b.h"
WATCHDOG_VALUE
};

uint32_t data_20210708c[] = {
#include "sample-20210708c.h"
WATCHDOG_VALUE
};

uint32_t data_20210709a3[] = {
#include "sample-20210709a3.h"
WATCHDOG_VALUE
};

uint32_t data_20210710[] = {
#include "sample-20210710.h"
WATCHDOG_VALUE
};

uint32_t data_20210711b[] = {
#include "sample-20210711b.h"
WATCHDOG_VALUE
};

struct smrty_msg *run_test(uint32_t **data) {
    struct smrty_msg *msg = NULL;
    do {
        uint32_t sample = **data;
        msg = process_transition(sample);
        if (sample != WATCHDOG_VALUE) {
            (*data)++;
        }
    } while (msg==NULL);
    return msg;
}

// helper for the "wake up" message
void assert_is_wakeup(struct smrty_msg *msg) {
    uint8_t wakeup_msg[] = { 0x01, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(wakeup_msg, (uint8_t*)msg, 8);
}

// helper for "report a reading" messages
void assert_is_report(struct smrty_msg *msg, uint8_t byte1, uint8_t byte2, uint8_t byte3, uint8_t byte4) {
    uint8_t expected[8] = {
        0x01, 0x01, byte1, byte2, byte3, byte4, 0x00, 0x00,
    };
    // compute checksum
    for (int i=0; i<7; i++) {
        expected[7] += expected[i];
    }
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, (uint8_t*)msg, 8);
}

void test_struct_is_packed(void) {
    TEST_ASSERT_EQUAL(8, sizeof(struct smrty_msg));
}

void test_20210706(void) {
    struct smrty_msg *msg;
    uint32_t *ptr = data_20210706;
    // Wake up, three times
    assert_is_wakeup(run_test(&ptr));
    assert_is_wakeup(run_test(&ptr));
    assert_is_wakeup(run_test(&ptr));
    // Now data message
    assert_is_report(run_test(&ptr), 0x0B, 0x00, 0xB4, 0x0D);
    assert_is_report(run_test(&ptr), 0x05, 0x00, 0x55, 0x01);
    assert_is_report(run_test(&ptr), 0x0E, 0x00, 0x00, 0x00);
}

void test_20210708a(void) {
    uint32_t *ptr = data_20210708a;
    // Wake up, three times
    assert_is_wakeup(run_test(&ptr));
    assert_is_wakeup(run_test(&ptr));
    assert_is_wakeup(run_test(&ptr));
    // Now data message
    assert_is_report(run_test(&ptr), 0x0B, 0x00, 0x87, 0x0D);
    assert_is_report(run_test(&ptr), 0x05, 0x00, 0x59, 0x01);
    assert_is_report(run_test(&ptr), 0x0E, 0x00, 0x00, 0x00);
}

void test_20210708b(void) {
    uint32_t *ptr = data_20210708b;
    // Wake up, three times
    assert_is_wakeup(run_test(&ptr));
    assert_is_wakeup(run_test(&ptr));
    assert_is_wakeup(run_test(&ptr));
    // Now data message
    assert_is_report(run_test(&ptr), 0x0B, 0x00, 0x81, 0x0D);
    assert_is_report(run_test(&ptr), 0x05, 0x00, 0x56, 0x01);
    assert_is_report(run_test(&ptr), 0x0E, 0x00, 0x00, 0x00);
}

void test_20210708c(void) {
    uint32_t *ptr = data_20210708c;
    // Wake up, three times
    assert_is_wakeup(run_test(&ptr));
    assert_is_wakeup(run_test(&ptr));
    assert_is_wakeup(run_test(&ptr));
    // Now data message
    assert_is_report(run_test(&ptr), 0x0B, 0x00, 0x75, 0x0D);
    assert_is_report(run_test(&ptr), 0x05, 0x00, 0x49, 0x01);
    assert_is_report(run_test(&ptr), 0x0E, 0x00, 0x00, 0x00);
}

void test_20210709a3(void) {
    uint32_t *ptr = data_20210709a3;
    // Wake up, three times
    assert_is_wakeup(run_test(&ptr));
    assert_is_wakeup(run_test(&ptr));
    assert_is_wakeup(run_test(&ptr));
    // Now data message: 38.0% soil moisture, 68.4 deg soil temp, 0.0 EC
    assert_is_report(run_test(&ptr), 0x0B, 0x00, 0xE0, 0x0E); // 0EE0
    // at around same time also saw: 44 01 for the 05 message
    assert_is_report(run_test(&ptr), 0x05, 0x00, 0x42, 0x01); // 0142
    assert_is_report(run_test(&ptr), 0x0E, 0x00, 0x00, 0x00);
    // Later: 38.6% soil moisture: 0B00 140F
    // 69.1 degrees: 0500 4A01
}

void test_20210710(void) {
    uint32_t *ptr = data_20210710;
    // Wake up, three times
    assert_is_wakeup(run_test(&ptr));
    assert_is_wakeup(run_test(&ptr));
    assert_is_wakeup(run_test(&ptr));
    // Now data message: 35.9% 70.7deg 0.0ec
    assert_is_report(run_test(&ptr), 0x0B, 0x00, 0x0F, 0x0E); // 0E0F
    assert_is_report(run_test(&ptr), 0x05, 0x00, 0x58, 0x01); // 0158
    assert_is_report(run_test(&ptr), 0x0E, 0x00, 0x00, 0x00);
}

void test_20210711b(void) {
    uint32_t *ptr = data_20210711b;
    // Wake up, three times
    assert_is_wakeup(run_test(&ptr));
    assert_is_wakeup(run_test(&ptr));
    assert_is_wakeup(run_test(&ptr));
    assert_is_report(run_test(&ptr), 0x0B, 0x00, 0xC8, 0x0F); // 0FC8
    assert_is_report(run_test(&ptr), 0x05, 0x00, 0x48, 0x01); // 0148
    assert_is_report(run_test(&ptr), 0x0E, 0x00, 0x00, 0x00);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_struct_is_packed);
    RUN_TEST(test_20210706);
    RUN_TEST(test_20210708a);
    RUN_TEST(test_20210708b);
    RUN_TEST(test_20210708c);
    RUN_TEST(test_20210709a3);
    RUN_TEST(test_20210710);
    RUN_TEST(test_20210711b);
    UNITY_END();

    return 0;
}
