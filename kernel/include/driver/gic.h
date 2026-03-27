/*
 * gic.h - Public API for the Generic Interrupt Controller (GICv2) driver.
 *
 * This file defines the interface and register pointers for interacting
 * with the system's interrupt controller.
 */

#ifndef PERSPICUA_DRIVER_GIC_H
#define PERSPICUA_DRIVER_GIC_H

/* GIC Distributor Register Pointers */
extern volatile unsigned int *gic_d_ctlr;
extern volatile unsigned int *gic_d_isenablern;
extern volatile unsigned char *gic_d_ipriorityr;
extern volatile unsigned char *gic_d_itargetsr;
extern volatile unsigned int *gic_d_sgir;

/* GIC CPU Interface Register Pointers */
extern volatile unsigned int *gic_c_ctlr;
extern volatile unsigned int *gic_c_pmr;
extern volatile unsigned int *gic_c_iar;
extern volatile unsigned int *gic_c_eoir;

/* The ARM physical timer IRQ (PPI) */
#define GIC_TIMER_IRQ 30

/*
 * gic_init - Initializes the GIC-400 distributor and CPU interface.
 * Discovers register bases from the hardware tree and enables core IRQs.
 */
void gic_init(void);

/*
 * gic_secondary_init - Initializes the GIC CPU interface for secondary cores.
 */
void gic_secondary_init(void);

/*
 * gic_send_panic_ipi - Sends a Software Generated Interrupt (SGI) to all other cores.
 * Used during kernel panic to halt secondary cores.
 */
void gic_send_panic_ipi(void);

#endif /* PERSPICUA_DRIVER_GIC_H */
