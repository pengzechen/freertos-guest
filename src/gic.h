#ifndef GIC_H
#define GIC_H

#include <stdint.h>

void gic_init(void);
void gic_enable_irq(uint32_t irq);
uint32_t gic_ack(void);
void gic_eoi(uint32_t irq);

#endif
