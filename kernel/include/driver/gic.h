/*
 * gic.h - Public API for the Generic Interrupt Controller (GICv2) driver.
 *
 * This header defines the interface and register pointers for interacting
 * with the ARM GIC-400 interrupt controller.
 */

#ifndef PERSPICUA_DRIVER_GIC_H
#define PERSPICUA_DRIVER_GIC_H

/* --- Constants and Macros --- */

#define GIC_TIMER_IRQ 30

/* --- GIC Distributor Registers --- */

extern volatile unsigned int *gic_d_ctlr;
extern volatile unsigned int *gic_d_isenablern;
extern volatile unsigned char *gic_d_ipriorityr;
extern volatile unsigned char *gic_d_itargetsr;
extern volatile unsigned int *gic_d_sgir;

/* --- GIC CPU Interface Registers --- */

extern volatile unsigned int *gic_c_ctlr;
extern volatile unsigned int *gic_c_pmr;
extern volatile unsigned int *gic_c_iar;
extern volatile unsigned int *gic_c_eoir;

/* --- Function Prototypes --- */

/*
 * gic_init - Discovers and initializes the GIC hardware.
 */
void gic_init(void);

/*
 * gic_secondary_init - Performs per-core GIC setup for secondary CPUs.
 */
void gic_secondary_init(void);

/*
 * gic_send_panic_ipi - Broadcasts a Software Generated Interrupt to halt other cores.
 */
void gic_send_panic_ipi(void);

#endif /* PERSPICUA_DRIVER_GIC_H */
