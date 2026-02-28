#ifndef _TIMER_H_
#define _TIMER_H_

void sleep_ms(unsigned int ms);
unsigned long get_system_time(void);

void timer_interrupt_init(void);
void timer_interrupt_reset(void);
void enable_interrupts(void);

#endif // _TIMER_H_
