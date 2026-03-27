/*
 * io.h - Memory-Mapped I/O (MMIO) primitives.
 *
 * This file provides inline functions for performing atomic, barrier-aware
 * memory-mapped register accesses.
 */

#ifndef PERSPICUA_LIBC_IO_H
#define PERSPICUA_LIBC_IO_H

#include "types.h"

/*
 * mmio_read - Reads a 32-bit value from a memory-mapped register and
 * ensures memory ordering with a load barrier.
 */
static inline uint32_t mmio_read(volatile uint32_t *reg)
{
    uint32_t val = *reg;
    __asm__ volatile("dmb ishld" ::: "memory");
    return val;
}

/*
 * mmio_write - Writes a 32-bit value to a memory-mapped register and
 * ensures memory ordering with a store barrier.
 */
static inline void mmio_write(volatile uint32_t *reg, uint32_t val)
{
    __asm__ volatile("dsb ishst" ::: "memory");
    *reg = val;
}

/*
 * mmio_read8 - Reads an 8-bit value from a memory-mapped register and
 * ensures memory ordering with a load barrier.
 */
static inline uint8_t mmio_read8(volatile uint8_t *reg)
{
    uint8_t val = *reg;
    __asm__ volatile("dsb ishld" ::: "memory");
    return val;
}

/*
 * mmio_write8 - Writes an 8-bit value to a memory-mapped register and
 * ensures memory ordering with a store barrier.
 */
static inline void mmio_write8(volatile uint8_t *reg, uint8_t val)
{
    __asm__ volatile("dsb ishst" ::: "memory");
    *reg = val;
}

#endif /* PERSPICUA_LIBC_IO_H */
