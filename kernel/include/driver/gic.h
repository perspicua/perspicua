/*
 * gic.h - Public API for the Generic Interrupt Controller (GICv2) driver.
 */

#ifndef PERSPICUA_DRIVER_GIC_H
#define PERSPICUA_DRIVER_GIC_H

#define GIC_TIMER_IRQ 30

extern volatile unsigned int *gic_d_ctlr;
extern volatile unsigned int *gic_d_isenablern;
extern volatile unsigned char *gic_d_ipriorityr;
extern volatile unsigned char *gic_d_itargetsr;
extern volatile unsigned int *gic_d_sgir;

extern volatile unsigned int *gic_c_ctlr;
extern volatile unsigned int *gic_c_pmr;
extern volatile unsigned int *gic_c_iar;
extern volatile unsigned int *gic_c_eoir;

/*
 * gic_enable_irq - Enables an SPI in the GIC distributor and routes it to CPU0.
 *
 * Called by device drivers from their probe functions after obtaining an IRQ
 * number via devm_get_irq(). Must be called after gic_probe() completes.
 */
void gic_enable_irq(unsigned int irq);

void gic_secondary_init(void);

void gic_send_panic_ipi(void);

#endif // PERSPICUA_DRIVER_GIC_H
