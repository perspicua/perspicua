/*
 * boot.S - Kernel entry: drop to EL1, park extra cores, build the boot page tables, enable the MMU.
 */

#include "mm/addr.h"

#define TCR_EL1_VALUE ((25 << 0) | (25 << 16) | (1 << 8) | (1 << 10) | (3 << 12) | (1 << 24) | \
                       (1 << 26) | (3 << 28) | (2UL << 30) | (1UL << 36))

.section ".text.boot"

.global _entry

    // ARM64 Image Header
    .balign 8
_entry:
    b real_entry          // branch to executable code
    .word 0               // reserved
    .quad 0x80000         // text_offset
    .quad 0               // image_size (0 = unknown/trust bootloader)
    .quad 0               // flags (0 = default)
    .quad 0               // res2
    .quad 0               // res3
    .quad 0               // res4
    .word 0x644d5241      // magic: "ARM\x64"
    .word 0               // res5

real_entry:
    mov x20, x0      // save the dtb pointer for later use in main()

    mrs x0, CurrentEL
    lsr x0, x0, #2
    cmp x0, #2
    b.ne el1_entry

    // currently in el2, drop to el1
    ldr x0, =(1 << 31)
    msr hcr_el2, x0

    // allow el1 to access physical counter and timer registers
    mrs x0, cnthctl_el2
    orr x0, x0, #3
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr

    ldr x0, =0x3c5
    msr spsr_el2, x0

    adr x0, el1_entry
    msr elr_el2, x0

    eret

el1_entry:
    // Clear TPIDR_EL1 so sched_current_task() returns NULL until a task is scheduled.
    msr tpidr_el1, xzr
    // CPACR_EL1.FPEN stays clear: nothing saves FP state, so FP faults instead of corrupting.
    msr cpacr_el1, xzr
    isb

    // Cores past CONFIG_NR_CPUS park here: no stack slot or per-core array was sized for them.
    mrs x0, mpidr_el1
    and x0, x0, #3
    cmp x0, #CONFIG_NR_CPUS
    b.hs _entry_loop

    // are we Core 0 or a Secondary Core?
    cmp x0, #0
    b.ne enable_mmu // cores 1, 2, 3 skip page table creation

build_page_tables:
    // core 0 only: build temp. boot page table
    adrp x0, boot_pgd   // pc-relative load gets the physical address
    adrp x1, boot_pmd

    // pgd[0] maps 0 to 1gb
    orr x2, x1, #3      // pte_valid | pte_table
    str x2, [x0]

    // pgd[1] and pgd[2] map 1gb to 3gb as RAM: mmu_init allocates from the top of RAM
    ldr x5, =0x701      // pte_valid | pte_block | pte_af | Inner-Shareable
    mov x2, #0x40000000
    orr x6, x2, x5
    str x6, [x0, #8]
    mov x2, #0x80000000
    orr x6, x2, x5
    str x6, [x0, #16]

    // pgd[3] maps 3gb to 4gb: RAM below 0xfc000000, peripherals above
    adrp x7, boot_pmd_periph
    orr x2, x7, #3
    str x2, [x0, #24]   // 3 * 8 bytes = offset 24 in pgd

    // populate pmd to identity map 0x0 to 1gb
    mov x2, #0          // address start
    mov x3, #512        // 512 entries
    mov x4, x1          // pmd pointer
    ldr x5, =0x701      // pte_valid | pte_block | pte_af | Inner-Shareable

map_ram_loop:
    orr x6, x2, x5
    str x6, [x4], #8
    add x2, x2, #(2 << 20) // +2mb
    subs x3, x3, #1
    b.ne map_ram_loop

    // populate pmd_periph to map 0xc0000000 to 0xffffffff
    mov x2, #0xC0000000 // address start (3gb)
    mov x3, #512
    mov x4, x7
    ldr x5, =0x701      // RAM below the peripheral window
    ldr x8, =0x705      // valid | block | AF | inner-shareable | AttrIndx 1 (device)
    mov x9, #BCM2711_PERIPH_BASE

map_periph_loop:
    cmp x2, x9
    csel x10, x5, x8, lo
    orr x6, x2, x10
    str x6, [x4], #8
    add x2, x2, #(2 << 20)
    subs x3, x3, #1
    b.ne map_periph_loop

enable_mmu:
    // MAIR_EL1: Attr0 normal write-back, Attr1 device nGnRE, Attr2 normal non-cacheable.
    ldr x0, =0x4404FF
    msr mair_el1, x0

    // TCR_EL1: 39-bit VAs both halves, 4 KB granules, WB inner-shareable walks, 16-bit ASIDs.
    ldr x0, =TCR_EL1_VALUE
    msr tcr_el1, x0

    adrp x0, boot_pgd
    msr ttbr0_el1, x0
    msr ttbr1_el1, x0
    isb

    // The walker must see the tables just written, or real hardware faults as the MMU comes on.
    dsb sy
    tlbi vmalle1
    dsb sy
    isb

    // invalidate I-cache before enabling it with potentially stale lines
    ic iallu
    dsb sy
    isb

    // SCTLR_EL1 from scratch, as firmware may leave WXN set: M, C, I and the RES1 bits.
    ldr x0, =0x30D01805
    orr x0, x0, #(1 << 3)  // Enable SA (SP Alignment Check for EL1)
    orr x0, x0, #(1 << 4)  // Enable SA0 (SP Alignment Check for EL0)
    msr sctlr_el1, x0
    isb

setup_stack:
    ldr x0, =vector_table
    msr vbar_el1, x0

    ldr x1, =_stack_top
    mrs x2, mpidr_el1
    and x2, x2, #3      // x2 = core id (0, 1, 2, or 3)

    mov x3, #0x10000    // x3 = 64KB
    mul x2, x2, x3      // core_offset = core_id * 64KB
    sub sp, x1, x2      // sp = _stack_top - core_offset

route_core:
    mrs x0, mpidr_el1
    and x0, x0, #3
    cmp x0, #0
    b.ne jump_secondary_main

clear_bss_loop:
    // core 0 only: clear bss
    ldr x0, =__bss_start
    ldr x1, =__bss_end
1:  cmp x0, x1
    b.hs jump_main
    str xzr, [x0], #8
    b 1b

jump_main:
    mov x0, x20         // pass the dtb pointer to main() as the first argument
    ldr x1, =main
    br x1               // Core 0 goes to main()

jump_secondary_main:
#ifdef CONFIG_SMP
    ldr x0, =secondary_main
    br x0               // Cores 1, 2, 3 go to secondary_main()
#else
    b _entry_loop       // No SMP: there is no secondary_main to jump to.
#endif

_entry_loop:
    wfe
    b _entry_loop

// allocate space for the boot tables
.section .data
.align 12
boot_pgd: .space 4096
boot_pmd: .space 4096
boot_pmd_periph: .space 4096
