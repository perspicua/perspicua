/*
 * addr.h - Macros for address space conversions and memory constants.
 */

#ifndef PERSPICUA_KERNEL_ADDR_H
#define PERSPICUA_KERNEL_ADDR_H

/*
 * KERNEL_VMA - The base virtual address where the kernel is mapped.
 * This offset is used to separate kernel space from user space.
 */
#define KERNEL_VMA 0xFFFFFF8000000000ULL

/*
 * V2P - Converts a kernel virtual address to a physical address.
 */
#define V2P(v) ((uint64_t)(uintptr_t)(v) - KERNEL_VMA)

/*
 * P2V - Converts a physical address to a kernel virtual address.
 */
#define P2V(p) (KERNEL_VMA + (uint64_t)(uintptr_t)(p))

#endif // PERSPICUA_KERNEL_ADDR_H
