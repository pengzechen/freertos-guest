#include "timer.h"
#include "gic.h"
#include "FreeRTOS.h"

#define PPI_VTIMER  27

static uint64_t timer_interval;
static uint64_t next_deadline;

static inline uint64_t read_cntfrq(void)
{
    uint64_t val;
    __asm volatile ("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

static inline uint64_t read_cntvct(void)
{
    uint64_t val;
    __asm volatile ("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

static inline void write_cntv_cval(uint64_t val)
{
    __asm volatile ("msr cntv_cval_el0, %0" :: "r"(val));
}

static inline void write_cntv_ctl(uint32_t val)
{
    __asm volatile ("msr cntv_ctl_el0, %0" :: "r"((uint64_t)val));
}

void timer_init(void)
{
    uint64_t freq = read_cntfrq();
    timer_interval = freq / configTICK_RATE_HZ;
}

void vSetupTickInterrupt(void)
{
    gic_enable_irq(PPI_VTIMER);

    next_deadline = read_cntvct() + timer_interval;
    write_cntv_cval(next_deadline);
    write_cntv_ctl(1);
}

void vClearTickInterrupt(void)
{
    next_deadline += timer_interval;
    uint64_t now = read_cntvct();
    if ((int64_t)(next_deadline - now) <= 0)
        next_deadline = now + timer_interval;
    write_cntv_cval(next_deadline);
}

uint64_t timer_last_deadline(void)
{
    return next_deadline;
}
