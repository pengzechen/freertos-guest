#include "timer.h"
#include "gic.h"
#include "FreeRTOS.h"

#ifndef TIMER_PPI
#define TIMER_PPI 30
#endif

static uint64_t timer_interval_counts;
#if TIMER_PPI == 30
static uint64_t timer_interval_ns;
#endif
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

#if TIMER_PPI == 30
static inline void write_timer_tval(uint64_t val)
{
    __asm volatile ("msr cntp_tval_el0, %0" :: "r"(val));
}

static inline void write_timer_ctl(uint32_t val)
{
    __asm volatile ("msr cntp_ctl_el0, %0" :: "r"((uint64_t)val));
}
#elif TIMER_PPI == 27
static inline void write_timer_cval(uint64_t val)
{
    __asm volatile ("msr cntv_cval_el0, %0" :: "r"(val));
}

static inline void write_timer_ctl(uint32_t val)
{
    __asm volatile ("msr cntv_ctl_el0, %0" :: "r"((uint64_t)val));
}
#else
#error "TIMER_PPI must be 27 or 30"
#endif

void timer_init(void)
{
    uint64_t freq = read_cntfrq();
    timer_interval_counts = freq / configTICK_RATE_HZ;
#if TIMER_PPI == 30
    timer_interval_ns = timer_interval_counts * 1000000000ULL / freq;
#endif
}

void vSetupTickInterrupt(void)
{
    gic_enable_irq(TIMER_PPI);

    next_deadline = read_cntvct() + timer_interval_counts;
#if TIMER_PPI == 30
    write_timer_tval(timer_interval_ns);
#else
    write_timer_cval(next_deadline);
#endif
    write_timer_ctl(1);
}

void vClearTickInterrupt(void)
{
    next_deadline += timer_interval_counts;
    uint64_t now = read_cntvct();
    if ((int64_t)(next_deadline - now) <= 0)
        next_deadline = now + timer_interval_counts;
#if TIMER_PPI == 30
    write_timer_tval(timer_interval_ns);
#else
    write_timer_cval(next_deadline);
#endif
}

uint64_t timer_last_deadline(void)
{
    return next_deadline;
}
