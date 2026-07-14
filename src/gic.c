#include "gic.h"

#define GICD_BASE  0x08000000UL
#define GICC_BASE  0x08010000UL

/* Distributor registers */
#define GICD_CTLR       (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_ISENABLER(n) (*(volatile uint32_t *)(GICD_BASE + 0x100 + 4 * (n)))
#define GICD_IPRIORITYR(n) (*(volatile uint8_t *)(GICD_BASE + 0x400 + (n)))
#define GICD_ITARGETSR(n) (*(volatile uint8_t *)(GICD_BASE + 0x800 + (n)))
#define GICD_ICFGR(n)   (*(volatile uint32_t *)(GICD_BASE + 0xC00 + 4 * (n)))

/* CPU interface registers (actually GICV via VMM remap) */
#define GICC_CTLR       (*(volatile uint32_t *)(GICC_BASE + 0x000))
#define GICC_PMR        (*(volatile uint32_t *)(GICC_BASE + 0x004))
#define GICC_BPR        (*(volatile uint32_t *)(GICC_BASE + 0x008))
#define GICC_IAR        (*(volatile uint32_t *)(GICC_BASE + 0x00C))
#define GICC_EOIR       (*(volatile uint32_t *)(GICC_BASE + 0x010))

void gic_init(void)
{
    /* Enable distributor */
    GICD_CTLR = 1;

    /* Enable CPU interface, allow all priorities */
    GICC_CTLR = 1;
    GICC_PMR  = 0xFF;
    GICC_BPR  = 0;
}

void gic_enable_irq(uint32_t irq)
{
    uint32_t reg = irq / 32;
    uint32_t bit = irq % 32;

    /* Set priority to lowest usable */
    GICD_IPRIORITYR(irq) = 0xA0;

    /* Target CPU 0 (for SGIs/PPIs this is banked, for SPIs set target) */
    if (irq >= 32)
        GICD_ITARGETSR(irq) = 0x01;

    /* Enable */
    GICD_ISENABLER(reg) = (1U << bit);
}

uint32_t gic_ack(void)
{
    return GICC_IAR;
}

void gic_eoi(uint32_t irq)
{
    GICC_EOIR = irq;
}
