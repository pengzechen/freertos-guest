#include "gic.h"

#ifndef GIC_VERSION
#define GIC_VERSION 2
#endif

#define GICD_BASE  0x08000000UL

#if GIC_VERSION == 2

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

#elif GIC_VERSION == 3

#define GICR_BASE  0x080a0000UL

/* Distributor registers */
#define GICD_CTLR          (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_ISENABLER(n)  (*(volatile uint32_t *)(GICD_BASE + 0x100 + 4 * (n)))
#define GICD_IPRIORITYR(n) (*(volatile uint8_t *)(GICD_BASE + 0x400 + (n)))
#define GICD_ICFGR(n)      (*(volatile uint32_t *)(GICD_BASE + 0xC00 + 4 * (n)))

/* Redistributor registers for CPU 0 on QEMU virt. */
#define GICR_CTLR          (*(volatile uint32_t *)(GICR_BASE + 0x0000))
#define GICR_WAKER         (*(volatile uint32_t *)(GICR_BASE + 0x0014))
#define GICR_IGROUPR0      (*(volatile uint32_t *)(GICR_BASE + 0x10000 + 0x080))
#define GICR_IGRPMODR0     (*(volatile uint32_t *)(GICR_BASE + 0x10000 + 0xD00))
#define GICR_ISENABLER0    (*(volatile uint32_t *)(GICR_BASE + 0x10000 + 0x100))
#define GICR_IPRIORITYR(n) (*(volatile uint8_t *)(GICR_BASE + 0x10000 + 0x400 + (n)))
#define GICR_ICFGR(n)      (*(volatile uint32_t *)(GICR_BASE + 0x10000 + 0xC00 + 4 * (n)))

#define GICD_CTLR_ENABLE_G1NS  (1U << 1)
#define GICD_CTLR_ARE_NS       (1U << 4)
#define GICR_WAKER_PROCESSOR_SLEEP (1U << 1)
#define GICR_WAKER_CHILDREN_ASLEEP (1U << 2)

static inline void dsb_sy(void)
{
    __asm volatile ("dsb sy" ::: "memory");
}

static inline void isb(void)
{
    __asm volatile ("isb" ::: "memory");
}

static inline void write_icc_sre(uint64_t val)
{
    __asm volatile ("msr S3_0_C12_C12_5, %0" :: "r"(val) : "memory");
    isb();
}

static inline void write_icc_pmr(uint64_t val)
{
    __asm volatile ("msr S3_0_C4_C6_0, %0" :: "r"(val) : "memory");
}

static inline void write_icc_bpr1(uint64_t val)
{
    __asm volatile ("msr S3_0_C12_C12_3, %0" :: "r"(val) : "memory");
}

static inline void write_icc_igrpen1(uint64_t val)
{
    __asm volatile ("msr S3_0_C12_C12_7, %0" :: "r"(val) : "memory");
    isb();
}

void gic_init(void)
{
    /* Wake CPU0 redistributor and enable non-secure Group 1 delivery. */
    GICR_WAKER &= ~GICR_WAKER_PROCESSOR_SLEEP;
    while (GICR_WAKER & GICR_WAKER_CHILDREN_ASLEEP) {
    }

    GICD_CTLR = GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1NS;
    GICR_CTLR = 0;
    GICR_IGROUPR0 = 0xFFFFFFFFU;
    GICR_IGRPMODR0 = 0;

    write_icc_sre(1);
    write_icc_pmr(0xFF);
    write_icc_bpr1(0);
    write_icc_igrpen1(1);
    dsb_sy();
    isb();
}

void gic_enable_irq(uint32_t irq)
{
    uint32_t bit = irq % 32;

    if (irq < 32) {
        GICR_IPRIORITYR(irq) = 0xA0;
        GICR_ICFGR(irq / 16) &= ~(0x3U << ((irq % 16) * 2));
        GICR_ISENABLER0 = 1U << bit;
    } else {
        uint32_t reg = irq / 32;

        GICD_IPRIORITYR(irq) = 0xA0;
        GICD_ICFGR(irq / 16) &= ~(0x3U << ((irq % 16) * 2));
        GICD_ISENABLER(reg) = 1U << bit;
    }
    dsb_sy();
    isb();
}

uint32_t gic_ack(void)
{
    uint64_t irq;

    __asm volatile ("mrs %0, S3_0_C12_C12_0" : "=r"(irq));
    return (uint32_t)irq;
}

void gic_eoi(uint32_t irq)
{
    __asm volatile ("msr S3_0_C12_C12_1, %0" :: "r"((uint64_t)irq) : "memory");
    isb();
}

#else
#error "GIC_VERSION must be 2 or 3"
#endif
