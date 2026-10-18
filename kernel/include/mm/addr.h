/*
 * addr.h - Macros for address space conversions and memory constants.
 */

#ifndef PERSPICUA_MM_ADDR_H
#define PERSPICUA_MM_ADDR_H

// The BCM2711 peripheral window's base; device memory up to the 4 GB the kernel maps.
#define BCM2711_PERIPH_BASE 0xFC000000

#ifndef __ASSEMBLER__
    #include <stdint.h>

    #define KERNEL_VMA 0xFFFFFF8000000000ULL

    #define V2P(v) ((uint64_t)(uintptr_t)(v) - KERNEL_VMA)

    #define P2V(p) (KERNEL_VMA + (uint64_t)(uintptr_t)(p))
#endif

#endif // PERSPICUA_MM_ADDR_H
