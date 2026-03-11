#include "test.h"
#include "types.h"

void test_types(void)
{
    TEST_SUITE_BEGIN("Types");

    // size of integer types
    TEST_ASSERT_EQ("sizeof uint8_t", sizeof(uint8_t), 1);
    TEST_ASSERT_EQ("sizeof uint16_t", sizeof(uint16_t), 2);
    TEST_ASSERT_EQ("sizeof uint32_t", sizeof(uint32_t), 4);
    TEST_ASSERT_EQ("sizeof uint64_t", sizeof(uint64_t), 8);
    TEST_ASSERT_EQ("sizeof int8_t", sizeof(int8_t), 1);
    TEST_ASSERT_EQ("sizeof int16_t", sizeof(int16_t), 2);
    TEST_ASSERT_EQ("sizeof int32_t", sizeof(int32_t), 4);
    TEST_ASSERT_EQ("sizeof int64_t", sizeof(int64_t), 8);
    TEST_PASS("integer type sizes");

    // pointer and size types (aarch64 lp64)
    TEST_ASSERT_EQ("sizeof size_t", sizeof(size_t), 8);
    TEST_ASSERT_EQ("sizeof ssize_t", sizeof(ssize_t), 8);
    TEST_ASSERT_EQ("sizeof uintptr_t", sizeof(uintptr_t), 8);
    TEST_ASSERT_EQ("sizeof ptrdiff_t", sizeof(ptrdiff_t), 8);
    TEST_ASSERT_EQ("sizeof void*", sizeof(void*), 8);
    TEST_PASS("pointer/size type sizes");

    // null
    TEST_ASSERT("NULL is zero", NULL == (void*)0);
    void* p = NULL;
    TEST_ASSERT("NULL ptr is false", !p);
    TEST_PASS("NULL definition");

    // limits
    TEST_ASSERT_EQ("UINT8_MAX", UINT8_MAX, 255);
    TEST_ASSERT_EQ("UINT16_MAX", UINT16_MAX, 65535);
    TEST_ASSERT_EQ("UINT32_MAX", (long)UINT32_MAX, (long)4294967295UL);
    TEST_ASSERT_EQ("INT8_MIN", INT8_MIN, -128);
    TEST_ASSERT_EQ("INT8_MAX", INT8_MAX, 127);
    TEST_ASSERT_EQ("INT16_MIN", INT16_MIN, -32768);
    TEST_ASSERT_EQ("INT16_MAX", INT16_MAX, 32767);
    TEST_PASS("integer limits");

    // signedness
    uint8_t u8 = 255;
    int8_t s8 = -1;
    TEST_ASSERT("uint8 wrap", u8 + 1 == 256);
    TEST_ASSERT("int8 neg", s8 < 0);
    TEST_PASS("signedness");

    TEST_SUITE_END("Types");
}
