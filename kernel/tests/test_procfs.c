/*
 * test_procfs.c - Every generated /proc file stays inside the buffer it is rendered into.
 */

#include "test.h"

#include <stddef.h>

#include "string.h"

#include "uapi/errno.h"
#include "uapi/fcntl.h"

#include "fs/procfs.h"
#include "fs/vfs.h"
#include "mm/heap.h"

// What procfs_generated_read renders into.
#define CONTENT_MAX 4096
#define SHORT       16

void test_procfs(void)
{
    TEST_SUITE_BEGIN("Procfs");

    char *full = heap_malloc(CONTENT_MAX);
    TEST_ASSERT("setup: render buffer", full != NULL);

    size_t files = 0;
    for (size_t i = 0; full; i++) {
        int len = procfs_test_generate(i, full, CONTENT_MAX);
        if (len == -ENOENT) {
            break;
        }
        files++;

        char name[64];
        snprintf(name, sizeof(name), "file %lu: renders below the cap", (unsigned long)i);
        TEST_ASSERT(name, len >= 0 && len < CONTENT_MAX - 1);

        // The returned length is what gets copied out, so it may never pass what landed.
        char small[SHORT + 8];
        memset(small, 0x7F, sizeof(small));
        int cut = procfs_test_generate(i, small, SHORT);
        snprintf(name, sizeof(name), "file %lu: a short buffer bounds the length",
                 (unsigned long)i);
        TEST_ASSERT(name, cut >= 0 && cut <= SHORT - 1 && (cut == 0 || small[cut] == '\0')
                              && (unsigned char)small[SHORT] == 0x7F);
    }
    TEST_ASSERT("every fixed file was rendered", files >= 11);

    // /proc/version is longer than the short buffer, so it is the one that must be cut.
    {
        char small[SHORT];
        TEST_ASSERT_EQ("an overlong file is cut at the buffer",
                       procfs_test_generate(0, small, SHORT), SHORT - 1);
    }

    // A process with no regions has an empty maps file, whatever the heap held before.
    {
        char *dirty = heap_malloc(CONTENT_MAX);
        if (dirty) {
            memset(dirty, 'Z', CONTENT_MAX);
            heap_free(dirty);
        }
        static char out[64];
        int fd = vfs_open("/proc/0/maps", O_RDONLY);
        TEST_ASSERT("open /proc/0/maps", fd >= 0);
        int n = fd >= 0 ? vfs_read(fd, out, sizeof(out)) : -1;
        if (fd >= 0) {
            vfs_close(fd);
        }
        TEST_ASSERT_EQ("a process with no regions has an empty maps file", n, 0);
    }

    heap_free(full);
    TEST_SUITE_END("Procfs");
}
