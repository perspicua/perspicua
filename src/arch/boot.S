.section ".text.boot"

.global _entry
_entry:
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
    // enable fpu/simd
    mov x0, #(3 << 20)
    msr cpacr_el1, x0
    isb

    // build temporary boot page tables
    adrp x0, boot_pgd   // pc-relative load gets the physical address
    adrp x1, boot_pmd

    // pgd[0] maps 0 to 1gb
    orr x2, x1, #3      // pte_valid | pte_table
    str x2, [x0]

    // pgd[3] maps 3gb to 4gb (peripherals like uart and gpio)
    adrp x7, boot_pmd_periph
    orr x2, x7, #3
    str x2, [x0, #24]   // 3 * 8 bytes = offset 24 in pgd

    // populate pmd to identity map 0x0 to 1gb
    mov x2, #0          // address start
    mov x3, #512        // 512 entries
    mov x4, x1          // pmd pointer
    ldr x5, =0x401      // pte_valid | pte_block | pte_af

map_ram_loop:
    orr x6, x2, x5
    str x6, [x4], #8
    add x2, x2, #(2 << 20) // +2mb
    subs x3, x3, #1
    b.ne map_ram_loop

    // populate pmd_periph to map 0xc0000000 to 0xffffffff
    // this allows uart_init() to access 0xfe201000 successfully
    mov x2, #0xC0000000 // address start (3gb)
    mov x3, #512
    mov x4, x7

map_periph_loop:
    orr x6, x2, x5
    str x6, [x4], #8
    add x2, x2, #(2 << 20)
    subs x3, x3, #1
    b.ne map_periph_loop

    // enable mmu
    ldr x0, =0xFF
    msr mair_el1, x0
    
    // tcr: 39-bit va for ttbr0 and ttbr1
    ldr x0, =(25 << 0) | (25 << 16) | (1 << 8) | (1 << 10) | (3 << 12) | (3 << 28)
    msr tcr_el1, x0
    
    adrp x0, boot_pgd
    msr ttbr0_el1, x0
    msr ttbr1_el1, x0   // both lower and higher half point to our boot table
    isb

    mrs x0, sctlr_el1
    orr x0, x0, #1      // enable mmu
    orr x0, x0, #4      // d-cache
    orr x0, x0, #4096   // i-cache
    msr sctlr_el1, x0
    isb

    // jump to the higher half
    ldr x0, =vector_table
    msr vbar_el1, x0

    ldr x1, =_stack_top
    mov sp, x1          // sp is now 0xffffff800008xxxx

    ldr x0, =__bss_start
    ldr x1, =__bss_end

clear_bss_loop:
    cmp x0, x1
    b.hs jump_main
    str xzr, [x0], #8
    b clear_bss_loop

jump_main:
    ldr x0, =main
    br x0               // absolute branch to 0xffffff800008xxxx

_entry_loop:
    wfe
    b _entry_loop

// allocate space for the boot tables
.section .data
.align 12
boot_pgd: .space 4096
boot_pmd: .space 4096
boot_pmd_periph: .space 4096
