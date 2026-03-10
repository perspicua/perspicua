#ifndef _IO_H_
#define _IO_H_

#include "lib/types.h"

static inline uint32_t mmio_read(volatile uint32_t* reg)
{
    uint32_t val = *reg;
    __asm__ volatile("dmb ishld" ::: "memory");
    return val;
}

static inline void mmio_write(volatile uint32_t* reg, uint32_t val)
{
    __asm__ volatile("dsb ishst" ::: "memory");
    *reg = val;
}

static inline uint8_t mmio_read8(volatile uint8_t* reg)
{
    uint8_t val = *reg;
    __asm__ volatile("dsb ishld" ::: "memory");
    return val;
}

static inline void mmio_write8(volatile uint8_t* reg, uint8_t val)
{
    __asm__ volatile("dsb ishst" ::: "memory");
    *reg = val;
}

#endif // _IO_H_
