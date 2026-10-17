/*
 * test_elf.c - The ELF loader maps a well-formed image and refuses malformed ones whole.
 */

#include <stddef.h>
#include <stdint.h>

#include "test.h"

#include "string.h"

#include "uapi/errno.h"
#include "uapi/fcntl.h"

#include "core/elf.h"
#include "fs/vfs.h"
#include "mm/addr.h"
#include "mm/mmu.h"
#include "mm/pmm.h"
#include "sched/process.h"

#define ELF_FILE  "/telf.tmp"
#define CODE_VA   0x100000UL
#define DATA_VA   0x101000UL
#define DATA_FILE 0x10

// Two segments: one code page, and a data page with a second page of BSS behind it.
struct elf_image {
    struct elf64_header ehdr;
    struct elf64_program_header ph[2];
    uint8_t pad[PAGE_SIZE - sizeof(struct elf64_header) - 2 * sizeof(struct elf64_program_header)];
    uint8_t code[PAGE_SIZE];
    uint8_t data[PAGE_SIZE];
};

static struct elf_image image;

static void image_reset(void)
{
    memset(&image, 0, sizeof(image));

    struct elf64_header *h = &image.ehdr;
    h->identity[ELF_IDENT_MAG0] = ELF_MAG0;
    h->identity[ELF_IDENT_MAG1] = ELF_MAG1;
    h->identity[ELF_IDENT_MAG2] = ELF_MAG2;
    h->identity[ELF_IDENT_MAG3] = ELF_MAG3;
    h->identity[ELF_IDENT_CLASS] = ELF_CLASS_64;
    h->identity[ELF_IDENT_DATA] = ELF_DATA_2LSB;
    h->identity[ELF_IDENT_VERSION] = 1;
    h->type = ELF_TYPE_EXEC;
    h->machine = ELF_MACHINE_AARCH64;
    h->version = 1;
    h->entry = CODE_VA;
    h->ph_offset = offsetof(struct elf_image, ph);
    h->eh_size = sizeof(struct elf64_header);
    h->ph_entry_size = sizeof(struct elf64_program_header);
    h->ph_num = 2;

    image.ph[0] = (struct elf64_program_header){
        .type = ELF_PROG_LOAD,
        .flags = 5,
        .offset = offsetof(struct elf_image, code),
        .vaddr = CODE_VA,
        .file_size = PAGE_SIZE,
        .mem_size = PAGE_SIZE,
    };
    image.ph[1] = (struct elf64_program_header){
        .type = ELF_PROG_LOAD,
        .flags = 6,
        .offset = offsetof(struct elf_image, data),
        .vaddr = DATA_VA,
        .file_size = DATA_FILE,
        .mem_size = 2 * PAGE_SIZE,
    };

    memset(image.code, 0xA5, sizeof(image.code));
    memset(image.data, 0x5A, sizeof(image.data));
}

static void bad_magic(void)
{
    image.ehdr.identity[ELF_IDENT_MAG1] = 'X';
}

static void bad_class(void)
{
    image.ehdr.identity[ELF_IDENT_CLASS] = 1;
}

static void bad_data(void)
{
    image.ehdr.identity[ELF_IDENT_DATA] = 2;
}

static void bad_type(void)
{
    image.ehdr.type = 3;
}

static void bad_machine(void)
{
    image.ehdr.machine = 62;
}

static void no_phdrs(void)
{
    image.ehdr.ph_num = 0;
}

static void too_many_phdrs(void)
{
    image.ehdr.ph_num = 65;
}

static void bad_phdr_size(void)
{
    image.ehdr.ph_entry_size = 32;
}

static void entry_zero(void)
{
    image.ehdr.entry = 0;
}

static void entry_high(void)
{
    image.ehdr.entry = USER_VA_BASE;
}

static void phdrs_past_eof(void)
{
    image.ehdr.ph_offset = 0x10000;
}

static void phdrs_negative(void)
{
    image.ehdr.ph_offset = 1UL << 63;
}

static void filesz_over_memsz(void)
{
    image.ph[1].mem_size = DATA_FILE / 2;
}

static void straddles_va_base(void)
{
    image.ph[1].vaddr = USER_VA_BASE - PAGE_SIZE;
}

static void wraps(void)
{
    image.ph[1].vaddr = ~0UL - PAGE_SIZE + 1;
}

static void data_past_eof(void)
{
    image.ph[1].offset = 0x10000;
}

static void data_negative(void)
{
    image.ph[1].offset = 1UL << 63;
}

static void data_on_code_page(void)
{
    image.ph[1].vaddr = CODE_VA + PAGE_SIZE / 2;
}

static void code_on_data_page(void)
{
    image.ph[0].flags = 6;
    image.ph[1].flags = 5;
    image.ph[1].vaddr = CODE_VA + PAGE_SIZE / 2;
}

static const struct {
    const char *name;
    void (*mutate)(void);
} bad_images[] = {
    {"bad magic", bad_magic},
    {"32-bit class", bad_class},
    {"big-endian", bad_data},
    {"not ET_EXEC", bad_type},
    {"another machine", bad_machine},
    {"no program headers", no_phdrs},
    {"more program headers than the cap", too_many_phdrs},
    {"program header size not the struct's", bad_phdr_size},
    {"entry 0", entry_zero},
    {"entry at USER_VA_BASE", entry_high},
    {"program headers past the end of the file", phdrs_past_eof},
    {"program header offset negative as an off_t", phdrs_negative},
    {"filesz over memsz", filesz_over_memsz},
    {"segment straddling USER_VA_BASE", straddles_va_base},
    {"segment wrapping the address space", wraps},
    {"segment data past the end of the file", data_past_eof},
    {"segment offset negative as an off_t", data_negative},
    {"data on a code page", data_on_code_page},
    {"code on a data page", code_on_data_page},
};

static int image_write(void)
{
    int fd = vfs_open(ELF_FILE, O_RDWR | O_CREAT);
    if (fd < 0) {
        return fd;
    }
    int n = vfs_pwrite(fd, &image, sizeof(image), 0);
    vfs_close(fd);
    return n == (int)sizeof(image) ? 0 : -1;
}

static const uint8_t *mapped(unsigned long *pgd, unsigned long va, unsigned long *flags)
{
    unsigned long pa = 0;
    if (!mmu_user_query(pgd, va, &pa, flags)) {
        return NULL;
    }
    return (const uint8_t *)P2V(pa);
}

void test_elf(void)
{
    TEST_SUITE_BEGIN("ELF Loader");

    // a well-formed image maps code read-only and executable, data writable, and zeroes the BSS
    {
        image_reset();
        TEST_ASSERT_EQ("setup: image written", image_write(), 0);

        unsigned long *pgd = mmu_create_user_pgd();
        uint64_t entry = 0;
        int rc = pgd ? elf_load(ELF_FILE, pgd, &entry) : -ENOMEM;
        TEST_ASSERT_EQ("well-formed image loads", rc, 0);
        TEST_ASSERT_EQ("entry point is the header's", entry, CODE_VA);

        unsigned long code_flags = 0, data_flags = 0, bss_flags = 0;
        const uint8_t *code = rc == 0 ? mapped(pgd, CODE_VA, &code_flags) : NULL;
        const uint8_t *data = rc == 0 ? mapped(pgd, DATA_VA, &data_flags) : NULL;
        const uint8_t *bss = rc == 0 ? mapped(pgd, DATA_VA + PAGE_SIZE, &bss_flags) : NULL;

        TEST_ASSERT("code: mapped with the file's bytes",
                    code && code[0] == 0xA5 && code[4095] == 0xA5);
        TEST_ASSERT("code: executable and read-only",
                    !(code_flags & MMU_UXN) && (code_flags & MMU_AP_RO));
        TEST_ASSERT("data: filesz bytes from the file, the rest zero",
                    data && data[DATA_FILE - 1] == 0x5A && data[DATA_FILE] == 0);
        TEST_ASSERT("data: writable, not executable",
                    (data_flags & MMU_UXN) && !(data_flags & MMU_AP_RO));
        TEST_ASSERT("bss: a zeroed page past filesz", bss && bss[0] == 0 && bss[4095] == 0);

        if (pgd) {
            mmu_destroy_user_pgd(pgd);
        }
    }

    // every malformed image is refused as not executable, and leaves no page behind
    for (size_t i = 0; i < sizeof(bad_images) / sizeof(bad_images[0]); i++) {
        image_reset();
        bad_images[i].mutate();
        if (image_write() != 0) {
            TEST_ASSERT(bad_images[i].name, 0);
            continue;
        }

        unsigned long before = pmm_get_free_pages();
        unsigned long *pgd = mmu_create_user_pgd();
        uint64_t entry = 0;
        int rc = pgd ? elf_load(ELF_FILE, pgd, &entry) : -ENOMEM;
        if (pgd) {
            mmu_destroy_user_pgd(pgd);
        }

        char leak[80];
        snprintf(leak, sizeof(leak), "%s: no page left behind", bad_images[i].name);
        TEST_ASSERT_EQ(bad_images[i].name, rc, -ENOEXEC);
        TEST_ASSERT_EQ(leak, pmm_get_free_pages(), before);
    }

    vfs_unlink(ELF_FILE);

    TEST_SUITE_END("ELF Loader");
}
