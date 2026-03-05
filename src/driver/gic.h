#ifndef _GIC_H_
#define _GIC_H_

extern volatile unsigned int* GICD_CTLR;
extern volatile unsigned int* GICD_ISENABLERn;
extern volatile unsigned char* GICD_IPRIORITYR;
extern volatile unsigned char* GICD_ITARGETSR;
extern volatile unsigned int* GICD_SGIR;

extern volatile unsigned int* GICC_CTLR;
extern volatile unsigned int* GICC_PMR;
extern volatile unsigned int* GICC_IAR;
extern volatile unsigned int* GICC_EOIR;

#define TIMER_IRQ 30

void gic_init(void);
void gic_secondary_init(void);
void gic_send_panic_ipi(void);
#endif // _GIC_H_
