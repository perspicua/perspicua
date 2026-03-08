#ifndef _ADDR_H_
#define _ADDR_H_

#define KERNEL_VMA 0xFFFFFF8000000000ULL
#define V2P(v) ((unsigned long)(v) - KERNEL_VMA)
#define P2V(p) ((unsigned long)(p) + KERNEL_VMA)

#endif // _ADDR_H_
