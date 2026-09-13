/*
 * cpu.h - Identity of the running CPU core.
 */

#ifndef PERSPICUA_ARCH_CPU_H
#define PERSPICUA_ARCH_CPU_H

#ifdef CONFIG_NR_CPUS
    #define CPU_MAX_CORES CONFIG_NR_CPUS
#else
    #define CPU_MAX_CORES 4
#endif

/*
 * cpu_id - Index of the calling core, always within 0..CPU_MAX_CORES-1.
 */
static inline int cpu_id(void)
{
    unsigned long mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (int)(mpidr & 0xFF) % CPU_MAX_CORES;
}

#endif // PERSPICUA_ARCH_CPU_H
