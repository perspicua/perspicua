/*
 * test_faultinject.c - One-shot allocation refusals walk the out-of-memory paths boot never takes.
 */

#include "test.h"

#include <stddef.h>
#include <stdint.h>

#include "string.h"

#include "uapi/errno.h"
#include "uapi/fcntl.h"
#include "uapi/mman.h"
#include "uapi/syscalls.h"

#include "core/elf.h"
#include "fs/vfs.h"
#include "mm/addr.h"
#include "mm/heap.h"
#include "mm/mmu.h"
#include "mm/pmm.h"
#include "mm/slab.h"
#include "sched/process.h"
#include "sched/sched.h"

// Where the path handed to the syscall probe below lives.
#define OOM_USER_VA 0x0000000051000000UL
#define OOM_PATH    "/bin/init.elf"

// Three pages under two L1 and three L2 entries, so a copy builds six tables.
#define OOM_SPREAD_PAGES 3
static const unsigned long oom_spread_va[OOM_SPREAD_PAGES] = {OOM_USER_VA, OOM_USER_VA + 0x200000,
                                                              OOM_USER_VA + 0x100000000UL};

// Past the last allocation each call makes, so the tail of the walk succeeds.
#define OOM_OPEN_MAX_NTH 16
#define OOM_PIPE_MAX_NTH 16
#define OOM_COPY_MAX_NTH 16
#define OOM_FORK_MAX_NTH 16

// fork's heap allocations are the process and then its task; refusing the second fails every round.
#define FORK_LAST_HEAP_ALLOC 2

#define OOM_MMAP_PAGES 4

static int spread_map(unsigned long *pgd, void *pages[OOM_SPREAD_PAGES])
{
    for (int i = 0; i < OOM_SPREAD_PAGES; i++) {
        pages[i] = pgd ? test_map_user_page(pgd, oom_spread_va[i], MMU_PAGE_USER_DATA) : NULL;
        if (!pages[i]) {
            return 0;
        }
    }
    return 1;
}

static int spread_held_once(void *const pages[OOM_SPREAD_PAGES])
{
    for (int i = 0; i < OOM_SPREAD_PAGES; i++) {
        if (pmm_page_refcount(pages[i]) != 1) {
            return 0;
        }
    }
    return 1;
}

static void unmap_region(unsigned long *pgd, uint64_t region, unsigned long pages)
{
    for (unsigned long i = 0; i < pages; i++) {
        mmu_user_unmap_page(pgd, region + i * PAGE_SIZE);
    }
    process_va_free(&process_table[0]->va, region);
}

static void dummy_task_entry(void) {}

void test_faultinject(void)
{
    TEST_SUITE_BEGIN("Fault Injection");

    // the arming fires once, on the allocation it names, and not before
    {
        heap_test_fail_nth(2);

        void *first = heap_malloc(64);
        void *second = heap_malloc(64);
        void *third = heap_malloc(64);

        TEST_ASSERT("the allocation before the armed one succeeds", first != NULL);
        TEST_ASSERT("the armed allocation is refused", second == NULL);
        TEST_ASSERT("the allocation after it succeeds again", third != NULL);

        heap_free(first);
        heap_free(third);
        heap_test_fail_nth(0);
    }

    // a size the heap would refuse anyway must not consume the arming
    {
        heap_test_fail_nth(1);

        TEST_ASSERT("an impossible size is still refused", heap_malloc(0) == NULL);

        void *p = heap_malloc(64);
        TEST_ASSERT("the arming was spent on the real request", p == NULL);

        heap_test_fail_nth(0);
        void *q = heap_malloc(64);
        TEST_ASSERT("disarming restores the heap", q != NULL);
        heap_free(q);
    }

    // a refused page allocation leaks nothing back to the pool
    {
        unsigned long before = pmm_get_free_pages();

        pmm_test_fail_nth(1);
        void *page = pmm_alloc_page();
        TEST_ASSERT("the armed page allocation is refused", page == NULL);

        pmm_test_fail_nth(0);
        TEST_ASSERT_EQ("a refused page allocation costs nothing", pmm_get_free_pages(), before);

        void *ok = pmm_alloc_page();
        TEST_ASSERT("the pool still serves after a refusal", ok != NULL);
        pmm_free_page(ok);

        TEST_ASSERT_EQ("the pool is whole again", pmm_get_free_pages(), before);
    }

    // refusing one page of an address space partway through unwinds the rest
    {
        unsigned long before = pmm_get_free_pages();

        for (unsigned long nth = 1; nth <= 4; nth++) {
            pmm_test_fail_nth(nth);

            unsigned long *pgd = mmu_create_user_pgd();
            if (pgd) {
                mmu_user_map_page(pgd, OOM_USER_VA, 0, MMU_PAGE_USER_DATA);
                mmu_destroy_user_pgd(pgd);
            }

            pmm_test_fail_nth(0);
        }

        TEST_ASSERT_EQ("a refused address space leaves no pages behind", pmm_get_free_pages(),
                       before);
    }

    // each allocation open makes, refused in turn, is -ENOMEM and leaves no descriptor behind
    {
        unsigned long *pgd = test_borrow_user_pgd();
        char *path = pgd ? test_map_user_page(pgd, OOM_USER_VA, MMU_PAGE_USER_DATA) : NULL;

        TEST_ASSERT("probe address space built", path != NULL);

        if (path) {
            strcpy(path, OOM_PATH);
            uint64_t args[6] = {OOM_USER_VA, O_RDONLY, 0, 0, 0, 0};

            int64_t first_fd = test_syscall(SYS_OPEN, args);
            TEST_ASSERT("open succeeds unarmed", first_fd >= 0);
            if (first_fd >= 0) {
                vfs_close((int)first_fd);
            }

            int enomem_ok = 1;
            int refusals = 0;
            int walked_past = 0;

            for (unsigned long nth = 1; nth <= OOM_OPEN_MAX_NTH; nth++) {
                heap_test_fail_nth(nth);
                int64_t ret = test_syscall(SYS_OPEN, args);
                heap_test_fail_nth(0);

                if (ret >= 0) {
                    vfs_close((int)ret);
                    walked_past = 1;
                } else if (ret == -ENOMEM) {
                    refusals++;
                } else {
                    pr_err("test: open with allocation %lu refused returned %ld [FAILED]\n", nth,
                           (long)ret);
                    enomem_ok = 0;
                }
            }

            TEST_ASSERT("open's allocations can be refused", refusals > 0);
            TEST_ASSERT("the walk reaches past open's last allocation", walked_past);
            TEST_ASSERT("every refusal is reported as -ENOMEM", enomem_ok);

            int64_t last_fd = test_syscall(SYS_OPEN, args);
            TEST_ASSERT_EQ("a refused open leaves no descriptor behind", last_fd, first_fd);
            if (last_fd >= 0) {
                vfs_close((int)last_fd);
            }
        }

        if (pgd) {
            test_release_user_pgd(pgd);
        }
    }

    // each page an image load takes, the page cache's included, refused in turn fails only the load
    {
        int ok = 1;
        int loaded = 0;

        for (unsigned long nth = 1; nth <= 64 && ok && !loaded; nth++) {
            unsigned long *pgd = mmu_create_user_pgd();
            if (!pgd) {
                ok = 0;
                break;
            }

            uint64_t entry = 0;
            pmm_test_fail_nth(nth);
            int r = elf_load(OOM_PATH, pgd, &entry);
            pmm_test_fail_nth(0);

            // The image is fine, so a refusal must not be reported as a bad one.
            if (r == 0) {
                loaded = 1;
            } else if (r == -ENOEXEC) {
                pr_err("test: elf_load with page %lu refused blamed the image [FAILED]\n", nth);
                ok = 0;
            }
            mmu_destroy_user_pgd(pgd);
        }

        TEST_ASSERT("every refusal fails the load without blaming the image", ok);
        TEST_ASSERT("the walk reaches a load that succeeds", loaded);
    }

    // a refused block split leaves the block intact; where RAM backs it, it may already be split
    if (!mmu_test_is_block_entry(P2V(0x80000000UL))) {
        pr_info("test: no unsplit 2 MB block at 0x80000000; refused split skipped\n");
    } else {
        unsigned long block_va = P2V(0x80000000UL);

        unsigned long query_pa = 0, query_flags = 0;
        TEST_ASSERT("block query succeeds before split attempt",
                    mmu_query(block_va, &query_pa, &query_flags));

        pmm_test_fail_nth(1);
        int unmap_ret = mmu_unmap_page(block_va);
        pmm_test_fail_nth(0);

        TEST_ASSERT_EQ("refused block split returns -ENOMEM", unmap_ret, -ENOMEM);
        TEST_ASSERT("block remains intact after refused split", mmu_test_is_block_entry(block_va));

        unsigned long after_pa = 0, after_flags = 0;
        TEST_ASSERT("block query succeeds after refused split",
                    mmu_query(block_va, &after_pa, &after_flags));
        TEST_ASSERT_EQ("block mapping unchanged after refused split", after_pa, query_pa);
    }

    // task creation fails with -ENOMEM without leaking pages or slab objects
    {
        unsigned long before_pages = pmm_get_free_pages();
        unsigned long before_slab = slab_get_used();

        heap_test_fail_nth(1);
        int ret = sched_create_task(dummy_task_entry);
        heap_test_fail_nth(0);
        TEST_ASSERT_EQ("refused task allocation reports -ENOMEM", ret, -ENOMEM);
        TEST_ASSERT_EQ("refused heap allocation leaves no slab objects", slab_get_used(),
                       before_slab);

        // warm up the task slab cache so PMM refusal targets the kernel stack
        void *warmup = heap_malloc(sizeof(struct task));
        heap_free(warmup);

        pmm_test_fail_nth(1);
        ret = sched_create_task(dummy_task_entry);
        pmm_test_fail_nth(0);
        TEST_ASSERT_EQ("refused stack allocation reports -ENOMEM", ret, -ENOMEM);
        TEST_ASSERT_EQ("refused stack allocation leaves no pages behind", pmm_get_free_pages(),
                       before_pages);
        TEST_ASSERT_EQ("refused stack allocation leaves no slab objects", slab_get_used(),
                       before_slab);
    }

    // copy-on-write fault resolution reports -ENOMEM when page allocation fails
    {
        unsigned long cow_before = pmm_get_free_pages();
        unsigned long *pgd = mmu_create_user_pgd();
        TEST_ASSERT("probe pgd for cow created", pgd != NULL);

        if (pgd) {
            void *page = pmm_alloc_page();
            TEST_ASSERT("source page allocated", page != NULL);
            if (page) {
                // A second reference makes the fault copy instead of taking the page over.
                pmm_hold_page(page);
                int mapped = mmu_user_map_page(pgd, OOM_USER_VA, V2P(page),
                                               MMU_PAGE_USER_DATA | MMU_PTE_COW | MMU_AP_RO);

                pmm_test_fail_nth(1);
                int cow_res = mmu_handle_cow(pgd, OOM_USER_VA);
                pmm_test_fail_nth(0);

                unsigned long pa = 0, flags = 0;
                int still_cow = mmu_user_query(pgd, OOM_USER_VA, &pa, &flags) && pa == V2P(page)
                                && (flags & MMU_PTE_COW);
                pmm_free_page(page);

                TEST_ASSERT_EQ("cow probe page mapped", mapped, 0);
                TEST_ASSERT_EQ("refused cow allocation reports -ENOMEM", cow_res, -ENOMEM);
                TEST_ASSERT("a refused cow fault leaves the shared page in place", still_cow);
            }
            mmu_destroy_user_pgd(pgd);
        }
        TEST_ASSERT_EQ("a refused cow fault leaks no pages", pmm_get_free_pages(), cow_before);
    }

    // each allocation pipe makes, refused in turn, is -ENOMEM and leaves no descriptor behind
    {
        unsigned long *pgd = test_borrow_user_pgd();
        int *pipefd = pgd ? (int *)test_map_user_page(pgd, OOM_USER_VA, MMU_PAGE_USER_DATA) : NULL;
        TEST_ASSERT("probe address space for pipe built", pipefd != NULL);

        if (pipefd) {
            uint64_t args[6] = {OOM_USER_VA, 0, 0, 0, 0, 0};
            int64_t ok_pipe = test_syscall(SYS_PIPE, args);
            int first_fd = ok_pipe == 0 ? pipefd[0] : -1;
            if (ok_pipe == 0) {
                vfs_close(pipefd[0]);
                vfs_close(pipefd[1]);
            }

            unsigned long (*const arm[])(unsigned long) = {heap_test_fail_nth, pmm_test_fail_nth};
            int enomem_ok = 1;
            int refusals = 0;
            int walked_past = 0;

            for (size_t a = 0; a < sizeof(arm) / sizeof(arm[0]); a++) {
                for (unsigned long nth = 1; nth <= OOM_PIPE_MAX_NTH; nth++) {
                    arm[a](nth);
                    int64_t ret = test_syscall(SYS_PIPE, args);
                    arm[a](0);

                    if (ret == 0) {
                        vfs_close(pipefd[0]);
                        vfs_close(pipefd[1]);
                        walked_past = 1;
                    } else if (ret == -ENOMEM) {
                        refusals++;
                    } else {
                        enomem_ok = 0;
                    }
                }
            }

            int64_t last_pipe = test_syscall(SYS_PIPE, args);
            int last_fd = last_pipe == 0 ? pipefd[0] : -1;
            if (last_pipe == 0) {
                vfs_close(pipefd[0]);
                vfs_close(pipefd[1]);
            }

            TEST_ASSERT("pipe succeeds unarmed", ok_pipe == 0);
            TEST_ASSERT("pipe's allocations can be refused", refusals > 0);
            TEST_ASSERT("the walk reaches past pipe's last allocation", walked_past);
            TEST_ASSERT("every pipe refusal is reported as -ENOMEM", enomem_ok);
            TEST_ASSERT_EQ("a refused pipe leaves no descriptor behind", last_fd, first_fd);
        }

        if (pgd) {
            test_release_user_pgd(pgd);
        }
    }

    // a copy refused at each table it builds unwinds them and leaves the parent writable
    {
        unsigned long *parent = mmu_create_user_pgd();
        void *pages[OOM_SPREAD_PAGES];
        int built = spread_map(parent, pages);
        TEST_ASSERT("probe address space for the copy built", built);

        if (built) {
            unsigned long before = pmm_get_free_pages();
            unsigned long *child = NULL;
            int refusals = 0;
            int clean = 1;

            for (unsigned long nth = 1; nth <= OOM_COPY_MAX_NTH; nth++) {
                pmm_test_fail_nth(nth);
                child = mmu_copy_user_pgd(parent);
                pmm_test_fail_nth(0);
                if (child) {
                    break;
                }
                refusals++;

                for (int i = 0; i < OOM_SPREAD_PAGES; i++) {
                    unsigned long flags = 0;
                    mmu_user_query(parent, oom_spread_va[i], NULL, &flags);
                    if (flags & (MMU_AP_RO | MMU_PTE_COW)) {
                        clean = 0;
                    }
                }
                if (!spread_held_once(pages) || pmm_get_free_pages() != before) {
                    pr_err("test: copy with page %lu refused leaked [FAILED]\n", nth);
                    clean = 0;
                }
            }

            TEST_ASSERT_EQ("a copy can be refused at each of its six tables", refusals, 6);
            TEST_ASSERT("the walk reaches a copy that succeeds", child != NULL);
            TEST_ASSERT("a refused copy leaks nothing and leaves the parent writable", clean);

            mmu_destroy_user_pgd(child);
            TEST_ASSERT_EQ("the copy that succeeded gives back what it took", pmm_get_free_pages(),
                           before);
        }
        mmu_destroy_user_pgd(parent);
    }

    // fork refused at each page allocation in turn, and then at its task, unwinds all of it
    {
        unsigned long *pgd = test_borrow_user_pgd();
        void *pages[OOM_SPREAD_PAGES];
        int built = spread_map(pgd, pages);
        TEST_ASSERT("probe address space for fork built", built);

        if (built) {
            uint64_t args[6] = {0, 0, 0, 0, 0, 0};

            // A first round grows whatever caches fork draws from, so the counts hold still.
            heap_test_fail_nth(FORK_LAST_HEAP_ALLOC);
            test_syscall(SYS_FORK, args);
            heap_test_fail_nth(0);

            unsigned long before_pages = pmm_get_free_pages();
            unsigned long before_slab = slab_get_used();
            int refusals = 0;
            int walked_past = 0;
            int clean = 1;

            for (unsigned long nth = 1; nth <= OOM_FORK_MAX_NTH && !walked_past; nth++) {
                heap_test_fail_nth(FORK_LAST_HEAP_ALLOC);
                pmm_test_fail_nth(nth);
                int64_t ret = test_syscall(SYS_FORK, args);
                walked_past = pmm_test_fail_nth(0) != 0;
                heap_test_fail_nth(0);
                refusals += !walked_past;

                if (ret != -ENOMEM || !spread_held_once(pages)
                    || pmm_get_free_pages() != before_pages || slab_get_used() != before_slab) {
                    pr_err("test: fork with page %lu refused returned %ld or leaked [FAILED]\n",
                           nth, (long)ret);
                    clean = 0;
                }
            }

            TEST_ASSERT_EQ("fork can be refused at its six tables and its stack", refusals, 7);
            TEST_ASSERT("the walk reaches fork's task allocation", walked_past);
            TEST_ASSERT("every refused fork returns -ENOMEM and leaks nothing", clean);
        }

        if (pgd) {
            test_release_user_pgd(pgd);
        }
    }

    // an mmap refused at each page in turn unmaps what it mapped and gives its range back
    {
        unsigned long *pgd = test_borrow_user_pgd();
        TEST_ASSERT("probe address space for mmap built", pgd != NULL);

        if (pgd) {
            uint64_t args[6] = {0,
                                OOM_MMAP_PAGES * PAGE_SIZE,
                                PROT_READ | PROT_WRITE,
                                MAP_ANONYMOUS | MAP_PRIVATE,
                                (uint64_t)-1,
                                0};

            // Mapped and unmapped once first, so the tables exist and refusals count data pages.
            int64_t region = test_syscall(SYS_MMAP, args);
            if (region > 0) {
                unmap_region(pgd, (uint64_t)region, OOM_MMAP_PAGES);
            }

            unsigned long before = pmm_get_free_pages();
            int64_t ret = -ENOMEM;
            int refusals = 0;
            int clean = 1;

            for (unsigned long nth = 1; nth <= OOM_MMAP_PAGES + 1 && ret < 0; nth++) {
                pmm_test_fail_nth(nth);
                ret = test_syscall(SYS_MMAP, args);
                pmm_test_fail_nth(0);

                if (ret == -ENOMEM) {
                    refusals++;
                    clean &= pmm_get_free_pages() == before;
                } else if (ret < 0) {
                    clean = 0;
                }
            }

            TEST_ASSERT("mmap succeeds unarmed", region > 0);
            TEST_ASSERT_EQ("an mmap can be refused at each of its pages", refusals, OOM_MMAP_PAGES);
            TEST_ASSERT("every refused mmap is -ENOMEM and leaks no pages", clean);
            TEST_ASSERT_EQ("a refused mmap gives its address range back", ret, region);

            if (ret > 0) {
                unmap_region(pgd, (uint64_t)ret, OOM_MMAP_PAGES);
            }
            TEST_ASSERT_EQ("the mmap that succeeded gives back its pages", pmm_get_free_pages(),
                           before);

            test_release_user_pgd(pgd);
        }
    }

    // nothing above may leave an arming behind for the suites that follow
    {
        void *p = heap_malloc(64);
        void *page = pmm_alloc_page();

        TEST_ASSERT("the heap is disarmed", p != NULL);
        TEST_ASSERT("the page allocator is disarmed", page != NULL);

        heap_free(p);
        pmm_free_page(page);
    }

    TEST_SUITE_END("Fault Injection");
}
