#ifndef _GIC_H_
#define _GIC_H_

#define GICD_BASE 0xFFFFFF80FF841000ULL
#define GICC_BASE 0xFFFFFF80FF842000ULL

#define GICD_CTLR (*(volatile unsigned int*)(GICD_BASE + 0x000))
#define GICD_ISENABLERn(n) (*(volatile unsigned int*)(GICD_BASE + 0x100 + 4 * (n)))
#define GICD_IPRIORITYR ((volatile unsigned char*)(GICD_BASE + 0x400))
#define GICD_ITARGETSR ((volatile unsigned char*)(GICD_BASE + 0x800))
#define GICD_SGIR      (*(volatile unsigned int*)(GICD_BASE + 0xF00))

#define GICC_CTLR (*(volatile unsigned int*)(GICC_BASE + 0x000))
#define GICC_PMR (*(volatile unsigned int*)(GICC_BASE + 0x004))
#define GICC_IAR (*(volatile unsigned int*)(GICC_BASE + 0x00C))
#define GICC_EOIR (*(volatile unsigned int*)(GICC_BASE + 0x010))

#define TIMER_IRQ 30
#define UART_IRQ 153

void gic_init(void);
void gic_secondary_init(void);
void gic_send_panic_ipi(void);
#endif // _GIC_H_
