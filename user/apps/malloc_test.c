#include "stdlib.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char **argv)
{
    printf("[MALLOC TEST] Starting...\n");

    void *ptr1 = malloc(1024);
    if (!ptr1) {
        printf("[MALLOC TEST] Failed to allocate 1024 bytes\n");
        return 1;
    }
    printf("[MALLOC TEST] Allocated 1024 bytes at %p\n", ptr1);
    memset(ptr1, 0xAA, 1024);

    void *ptr2 = malloc(2048);
    if (!ptr2) {
        printf("[MALLOC TEST] Failed to allocate 2048 bytes\n");
        return 1;
    }
    printf("[MALLOC TEST] Allocated 2048 bytes at %p\n", ptr2);
    memset(ptr2, 0xBB, 2048);

    // Verify content
    for (int i = 0; i < 1024; i++) {
        if (((unsigned char *)ptr1)[i] != 0xAA) {
            printf("[MALLOC TEST] Verification failed at ptr1[%d]\n", i);
            return 1;
        }
    }
    for (int i = 0; i < 2048; i++) {
        if (((unsigned char *)ptr2)[i] != 0xBB) {
            printf("[MALLOC TEST] Verification failed at ptr2[%d]\n", i);
            return 1;
        }
    }
    printf("[MALLOC TEST] Verification PASSED\n");

    free(ptr1);
    printf("[MALLOC TEST] Freed ptr1\n");

    void *ptr3 = malloc(512);
    printf("[MALLOC TEST] Allocated 512 bytes at %p (should reuse part of ptr1)\n", ptr3);

    free(ptr2);
    free(ptr3);
    printf("[MALLOC TEST] Freed ptr2 and ptr3\n");

    // Test large allocation
    void *ptr4 = malloc(2 * 1024 * 1024); // 2MB
    if (!ptr4) {
        printf("[MALLOC TEST] Failed to allocate 2MB\n");
        return 1;
    }
    printf("[MALLOC TEST] Allocated 2MB at %p\n", ptr4);
    free(ptr4);

    printf("[MALLOC TEST] All tests PASSED\n");
    return 0;
}
