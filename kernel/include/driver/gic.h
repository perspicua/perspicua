/*
 * gic.h - Public API for the Generic Interrupt Controller (GICv2) driver.
 */

#ifndef PERSPICUA_DRIVER_GIC_H
#define PERSPICUA_DRIVER_GIC_H

#define GIC_TIMER_IRQ 30

// The exception path acknowledges and ends interrupts through these two.
extern volatile unsigned int *gic_c_iar;
extern volatile unsigned int *gic_c_eoir;

// Enables an SPI and routes it to CPU0; drivers call it from probe, after gic_probe().
void gic_enable_irq(unsigned int irq);

// Masks an SPI at the distributor; called before free_irq(), so nothing fires at a cleared entry.
void gic_disable_irq(unsigned int irq);

void gic_secondary_init(void);

void gic_send_panic_ipi(void);

// Raises software-generated interrupt sgi (1-15; 0 is the panic IPI) on the calling core.
void gic_send_sgi_self(unsigned int sgi);

#endif // PERSPICUA_DRIVER_GIC_H
