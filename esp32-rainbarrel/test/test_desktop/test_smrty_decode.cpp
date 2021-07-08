#include <unity.h>
#include "smrty_decode.h"

void test_decode(void) {
    TEST_ASSERT_EQUAL(8, sizeof(struct smrty_msg));
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_decode);
    UNITY_END();

    return 0;
}
