#include "test.h"
#include "../lib/string.h"
#include "../lib/types.h"

void test_string(void)
{
    TEST_SUITE_BEGIN("String");

    // strlen
    TEST_ASSERT_EQ("strlen empty", strlen(""), 0);
    TEST_ASSERT_EQ("strlen 1 char", strlen("x"), 1);
    TEST_ASSERT_EQ("strlen hello", strlen("hello"), 5);
    TEST_ASSERT_EQ("strlen spaces", strlen("a b c"), 5);
    TEST_ASSERT_EQ("strlen embedded", strlen("ab\0cd"), 2);
    TEST_PASS("strlen");

    // strcmp
    TEST_ASSERT("strcmp equal", strcmp("abc", "abc") == 0);
    TEST_ASSERT("strcmp empty", strcmp("", "") == 0);
    TEST_ASSERT("strcmp less", strcmp("abc", "abd") < 0);
    TEST_ASSERT("strcmp greater", strcmp("abd", "abc") > 0);
    TEST_ASSERT("strcmp prefix short", strcmp("ab", "abc") < 0);
    TEST_ASSERT("strcmp prefix long", strcmp("abc", "ab") > 0);
    TEST_ASSERT("strcmp empty vs str", strcmp("", "a") < 0);
    TEST_ASSERT("strcmp str vs empty", strcmp("a", "") > 0);
    TEST_ASSERT("strcmp single eq", strcmp("z", "z") == 0);
    TEST_PASS("strcmp");

    // strncmp
    TEST_ASSERT("strncmp equal n=3", strncmp("abcX", "abcY", 3) == 0);
    TEST_ASSERT("strncmp diff n=4", strncmp("abcX", "abcY", 4) != 0);
    TEST_ASSERT("strncmp zero count", strncmp("abc", "xyz", 0) == 0);
    TEST_ASSERT("strncmp prefix", strncmp("abc", "abcdef", 3) == 0);
    TEST_ASSERT("strncmp short str", strncmp("ab", "ab", 5) == 0);
    TEST_ASSERT("strncmp n=1 eq", strncmp("ax", "ay", 1) == 0);
    TEST_ASSERT("strncmp n=1 diff", strncmp("x", "y", 1) < 0);
    TEST_PASS("strncmp");

    // strcpy
    {
        char buf[64];
        char* ret = strcpy(buf, "hello");
        TEST_ASSERT("strcpy contents", strcmp(buf, "hello") == 0);
        TEST_ASSERT("strcpy returns dst", ret == buf);

        strcpy(buf, "");
        TEST_ASSERT("strcpy empty", strcmp(buf, "") == 0);
        TEST_ASSERT_EQ("strcpy empty len", strlen(buf), 0);
    }
    TEST_PASS("strcpy");

    // strncpy
    {
        char buf[16];
        memset(buf, 'X', sizeof(buf));

        strncpy(buf, "hi", 16);
        TEST_ASSERT("strncpy contents", strcmp(buf, "hi") == 0);

        memset(buf, 'X', sizeof(buf));
        strncpy(buf, "hello world", 5);
        TEST_ASSERT("strncpy trunc", memcmp(buf, "hello", 5) == 0);

        memset(buf, 'X', sizeof(buf));
        strncpy(buf, "abc", 3);
        TEST_ASSERT("strncpy exact", memcmp(buf, "abc", 3) == 0);
    }
    TEST_PASS("strncpy");

    // strcat
    {
        char buf[64];
        strcpy(buf, "hello");
        char* ret = strcat(buf, " world");
        TEST_ASSERT("strcat contents", strcmp(buf, "hello world") == 0);
        TEST_ASSERT("strcat returns dst", ret == buf);

        strcpy(buf, "abc");
        strcat(buf, "");
        TEST_ASSERT("strcat empty src", strcmp(buf, "abc") == 0);

        buf[0] = '\0';
        strcat(buf, "xyz");
        TEST_ASSERT("strcat empty dest", strcmp(buf, "xyz") == 0);

        strcpy(buf, "a");
        strcat(buf, "b");
        strcat(buf, "c");
        TEST_ASSERT("strcat chained", strcmp(buf, "abc") == 0);
    }
    TEST_PASS("strcat");

    // strncat
    {
        char buf[64];
        strcpy(buf, "hello");
        strncat(buf, " world!", 6);
        TEST_ASSERT("strncat limited", strcmp(buf, "hello world") == 0);

        strcpy(buf, "abc");
        strncat(buf, "defghij", 0);
        TEST_ASSERT("strncat zero n", strcmp(buf, "abc") == 0);

        strcpy(buf, "abc");
        strncat(buf, "de", 10);
        TEST_ASSERT("strncat n > len", strcmp(buf, "abcde") == 0);
    }
    TEST_PASS("strncat");

    // strchr
    {
        const char* s = "hello world";
        TEST_ASSERT("strchr found 'l'", strchr(s, 'l') == s + 2);
        TEST_ASSERT("strchr found 'h'", strchr(s, 'h') == s);
        TEST_ASSERT("strchr found 'd'", strchr(s, 'd') == s + 10);
        TEST_ASSERT("strchr not found", strchr(s, 'z') == NULL);
        TEST_ASSERT("strchr empty str", strchr("", 'a') == NULL);
    }
    TEST_PASS("strchr");

    // strrchr
    {
        const char* s = "hello world";
        TEST_ASSERT("strrchr last 'l'", strrchr(s, 'l') == s + 9);
        TEST_ASSERT("strrchr first 'h'", strrchr(s, 'h') == s);
        TEST_ASSERT("strrchr not found", strrchr(s, 'z') == NULL);
        TEST_ASSERT("strrchr single", strrchr("x", 'x') != NULL);
    }
    TEST_PASS("strrchr");

    // strstr
    {
        const char* s = "hello world hello";
        TEST_ASSERT("strstr found", strstr(s, "world") == s + 6);
        TEST_ASSERT("strstr at begin", strstr(s, "hello") == s);
        TEST_ASSERT("strstr at end", strstr("abcxyz", "xyz") != NULL);
        TEST_ASSERT("strstr not found", strstr(s, "xyz") == NULL);
        TEST_ASSERT("strstr empty needle", strstr(s, "") == s);
        TEST_ASSERT("strstr same", strstr("abc", "abc") != NULL);
        TEST_ASSERT("strstr partial match", strstr("aab", "ab") != NULL);
        TEST_ASSERT("strstr longer needle", strstr("ab", "abc") == NULL);
    }
    TEST_PASS("strstr");

    // memset
    {
        char buf[128];
        memset(buf, 0xAA, sizeof(buf));
        int ok = 1;
        for (int i = 0; i < 128; i++)
            if ((unsigned char)buf[i] != 0xAA)
            {
                ok = 0;
                break;
            }
        TEST_ASSERT("memset 0xAA fill", ok);

        memset(buf, 0, sizeof(buf));
        ok = 1;
        for (int i = 0; i < 128; i++)
            if (buf[i] != 0)
            {
                ok = 0;
                break;
            }
        TEST_ASSERT("memset zero fill", ok);

        char one = 'X';
        memset(&one, 'Y', 1);
        TEST_ASSERT("memset single", one == 'Y');

        char odd[13];
        memset(odd, 0x55, 13);
        ok = 1;
        for (int i = 0; i < 13; i++)
            if ((unsigned char)odd[i] != 0x55)
            {
                ok = 0;
                break;
            }
        TEST_ASSERT("memset odd size", ok);

        char rv[4];
        void* ret = memset(rv, 'Z', 4);
        TEST_ASSERT("memset returns dest", ret == rv);
    }
    TEST_PASS("memset");

    // memcpy
    {
        char src[] = "copy me!";
        char dst[16];
        void* ret = memcpy(dst, src, 9);
        TEST_ASSERT("memcpy contents", strcmp(dst, "copy me!") == 0);
        TEST_ASSERT("memcpy returns dst", ret == dst);

        char a = 'A', b = 'B';
        memcpy(&b, &a, 1);
        TEST_ASSERT("memcpy single byte", b == 'A');

        char big_src[256], big_dst[256];
        for (int i = 0; i < 256; i++)
            big_src[i] = (char)(i & 0xFF);
        memcpy(big_dst, big_src, 256);
        TEST_ASSERT("memcpy 256 bytes", memcmp(big_dst, big_src, 256) == 0);

        char os[7] = "abcdef";
        char od[7];
        memcpy(od, os, 7);
        TEST_ASSERT("memcpy odd size", memcmp(od, os, 7) == 0);
    }
    TEST_PASS("memcpy");

    // memcmp
    {
        uint8_t a[] = {1, 2, 3, 4, 5};
        uint8_t b[] = {1, 2, 3, 4, 5};
        uint8_t c[] = {1, 2, 3, 4, 6};
        uint8_t d[] = {1, 2, 3, 4, 4};

        TEST_ASSERT("memcmp equal", memcmp(a, b, 5) == 0);
        TEST_ASSERT("memcmp less", memcmp(a, c, 5) < 0);
        TEST_ASSERT("memcmp greater", memcmp(a, d, 5) > 0);
        TEST_ASSERT("memcmp partial eq", memcmp(a, c, 4) == 0);
        TEST_ASSERT("memcmp single eq", memcmp(a, b, 1) == 0);
        TEST_ASSERT("memcmp single diff", memcmp("\x00", "\x01", 1) < 0);
    }
    TEST_PASS("memcmp");

    // memmove
    {
        char buf1[] = "abcdefgh";
        memmove(buf1 + 2, buf1, 4);
        TEST_ASSERT("memmove fwd overlap", memcmp(buf1, "ababcd", 6) == 0);

        char buf2[] = "abcdefgh";
        memmove(buf2, buf2 + 2, 4);
        TEST_ASSERT("memmove bwd overlap", memcmp(buf2, "cdef", 4) == 0);

        char src[] = "hello";
        char dst[8];
        memmove(dst, src, 6);
        TEST_ASSERT("memmove no overlap", strcmp(dst, "hello") == 0);

        char x = 'A';
        memmove(&x, &x, 1);
        TEST_ASSERT("memmove self", x == 'A');

        char rv_src[4] = "abc";
        char rv_dst[4];
        void* ret = memmove(rv_dst, rv_src, 4);
        TEST_ASSERT("memmove returns dst", ret == rv_dst);
    }
    TEST_PASS("memmove");

    TEST_SUITE_END("String");
}
