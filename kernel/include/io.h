/*
 * io.h - Memory-Mapped I/O (MMIO) primitives.
 *
 * Device accesses to one peripheral stay in order without barriers. The plain
 * accessors also order an access against normal memory, which a DMA master or
 * another core may read; the _relaxed ones do not, for device-only sequences.
 */

#ifndef PERSPICUA_IO_H
#define PERSPICUA_IO_H

#include <stdint.h>

static inline uint32_t mmio_read_relaxed(const volatile uint32_t *reg)
{
    return *reg;
}

static inline void mmio_write_relaxed(volatile uint32_t *reg, uint32_t val)
{
    *reg = val;
}

// Later normal-memory reads cannot pass this load.
static inline uint32_t mmio_read(const volatile uint32_t *reg)
{
    uint32_t val = *reg;
    __asm__ volatile("dmb oshld" ::: "memory");
    return val;
}

// Earlier normal-memory stores are observable before this one.
static inline void mmio_write(volatile uint32_t *reg, uint32_t val)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *reg = val;
}

static inline uint8_t mmio_read8(const volatile uint8_t *reg)
{
    uint8_t val = *reg;
    __asm__ volatile("dmb oshld" ::: "memory");
    return val;
}

static inline void mmio_write8(volatile uint8_t *reg, uint8_t val)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *reg = val;
}

#endif // PERSPICUA_IO_H
