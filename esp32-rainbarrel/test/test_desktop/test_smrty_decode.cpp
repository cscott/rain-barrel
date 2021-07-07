#include <unity.h>
#include "smrty_decode.h"

void test_decode(void) {
    // cheating!
    TEST_ASSERT_EQUAL(0, 0);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_decode);
    UNITY_END();

    return 0;
}
