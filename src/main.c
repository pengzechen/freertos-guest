#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "gic.h"
#include "timer.h"
#include "uart.h"

extern void FreeRTOS_Tick_Handler(void);

static uint64_t cntfrq;

static inline uint64_t read_cntvct(void)
{
    uint64_t v;
    __asm volatile ("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static inline uint64_t read_cntfrq(void)
{
    uint64_t v;
    __asm volatile ("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline uint64_t counts_to_ns(uint64_t c)
{
    return c * 1000000000ULL / cntfrq;
}

/* ── Statistics collector ── */

struct stats {
    uint64_t min, max, sum;
    uint32_t count;
};

static void stats_init(struct stats *s)
{
    s->min = UINT64_MAX;
    s->max = 0;
    s->sum = 0;
    s->count = 0;
}

static void stats_record(struct stats *s, uint64_t val)
{
    if (val < s->min) s->min = val;
    if (val > s->max) s->max = val;
    s->sum += val;
    s->count++;
}

static void stats_print(const char *name, struct stats *s)
{
    if (s->count == 0) {
        uart_printf("  [%s] no data\n", name);
        return;
    }
    uint64_t avg_ns = counts_to_ns(s->sum / s->count);
    uint64_t min_ns = counts_to_ns(s->min);
    uint64_t max_ns = counts_to_ns(s->max);
    uint64_t jitter_ns = counts_to_ns(s->max - s->min);
    uart_printf("  [%s] n=%u  avg=%lu  min=%lu  max=%lu  jitter=%lu ns\n",
                name, s->count,
                (unsigned long)avg_ns, (unsigned long)min_ns,
                (unsigned long)max_ns, (unsigned long)jitter_ns);
}

/* ── Benchmark config ── */

#define ITERATIONS 10000

/* ────────────────────────────────────────────────────────────────────
 * Test 1 — Task Switch Time
 *
 * Two equal-priority tasks ping-pong with direct notifications.  The
 * measuring task blocks immediately after waking its peer, forcing a real
 * task switch without relying on tick-driven time slicing.
 * Measures: notification + scheduler + cooperative context-switch overhead.
 * ──────────────────────────────────────────────────────────────────── */

static struct stats sw_stats;
static volatile uint64_t sw_ts;
static TaskHandle_t hSwA;
static TaskHandle_t hSwB;
static volatile int sw_done;

static void vTaskSwA(void *p)
{
    (void)p;
    for (uint32_t i = 0; i < ITERATIONS; i++) {
        sw_ts = read_cntvct();
        xTaskNotifyGive(hSwB);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    vTaskSuspend(NULL);
}

static void vTaskSwB(void *p)
{
    (void)p;
    for (uint32_t i = 0; i < ITERATIONS; i++) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint64_t now = read_cntvct();
        stats_record(&sw_stats, now - sw_ts);
        xTaskNotifyGive(hSwA);
    }
    sw_done = 1;
    vTaskSuspend(NULL);
}

/* ────────────────────────────────────────────────────────────────────
 * Test 2 — Task Preemption Time
 *
 * Low-priority task sends notification; high-priority task wakes
 * and preempts immediately.
 * Measures: notification + scheduler + preemptive context-switch.
 * ──────────────────────────────────────────────────────────────────── */

static struct stats pre_stats;
static volatile uint64_t pre_ts;
static TaskHandle_t hPreHigh;
static volatile int pre_done;

static void vTaskPreHigh(void *p)
{
    (void)p;
    for (uint32_t i = 0; i < ITERATIONS; i++) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint64_t now = read_cntvct();
        stats_record(&pre_stats, now - pre_ts);
    }
    pre_done = 1;
    vTaskSuspend(NULL);
}

static void vTaskPreLow(void *p)
{
    (void)p;
    vTaskDelay(pdMS_TO_TICKS(10));
    for (uint32_t i = 0; i < ITERATIONS; i++) {
        pre_ts = read_cntvct();
        xTaskNotifyGive(hPreHigh);
    }
    vTaskSuspend(NULL);
}

/* ────────────────────────────────────────────────────────────────────
 * Test 3 — Interrupt Latency + Tick Jitter
 *
 * Measured inside vApplicationIRQHandler (before FreeRTOS_Tick_Handler
 * increments next_deadline).
 *
 * IRQ latency  = cntvct_now − timer_deadline
 *   (timer fire → ISR entry; for kvmm includes VM exit/entry)
 * Tick delta   = cntvct_now − cntvct_prev_isr
 *   (interval stability; ideal = CNTFRQ / TICK_RATE_HZ)
 * ──────────────────────────────────────────────────────────────────── */

static struct stats irq_stats;
static struct stats tick_stats;
static volatile int irq_collecting;
static uint64_t last_irq_time;

/* ────────────────────────────────────────────────────────────────────
 * Test 4 — Semaphore Shuffle Time
 *
 * Low-priority task gives a binary semaphore; high-priority task
 * (blocked on take) wakes and preempts.
 * Measures: semaphore release + scheduler + context-switch.
 * ──────────────────────────────────────────────────────────────────── */

static struct stats sem_stats;
static volatile uint64_t sem_ts;
static SemaphoreHandle_t bench_sem;
static volatile int sem_done;

static void vTaskSemTaker(void *p)
{
    (void)p;
    for (uint32_t i = 0; i < ITERATIONS; i++) {
        xSemaphoreTake(bench_sem, portMAX_DELAY);
        uint64_t now = read_cntvct();
        stats_record(&sem_stats, now - sem_ts);
    }
    sem_done = 1;
    vTaskSuspend(NULL);
}

static void vTaskSemGiver(void *p)
{
    (void)p;
    vTaskDelay(pdMS_TO_TICKS(10));
    for (uint32_t i = 0; i < ITERATIONS; i++) {
        sem_ts = read_cntvct();
        xSemaphoreGive(bench_sem);
    }
    vTaskSuspend(NULL);
}

/* ── IRQ handler ── */

void vApplicationIRQHandler(uint32_t ulICCIAR)
{
    uint32_t irq = ulICCIAR & 0x3FF;

    if (irq == 27) {
        if (irq_collecting) {
            uint64_t now = read_cntvct();
            uint64_t deadline = timer_last_deadline();
            if (now >= deadline)
                stats_record(&irq_stats, now - deadline);
            if (last_irq_time != 0)
                stats_record(&tick_stats, now - last_irq_time);
            last_irq_time = now;
        }
        FreeRTOS_Tick_Handler();
    }
}

/* ── Control task — runs each test sequentially ── */

static void vTaskControl(void *p)
{
    (void)p;
    uint64_t expected_interval = cntfrq / configTICK_RATE_HZ;

    uart_puts("\n===== Rhealstone Benchmark =====\n");
    uart_printf("  CNTFRQ      = %lu Hz\n", (unsigned long)cntfrq);
    uart_printf("  Tick rate   = %u Hz (interval = %lu counts = %lu ns)\n",
                configTICK_RATE_HZ,
                (unsigned long)expected_interval,
                (unsigned long)counts_to_ns(expected_interval));
    uart_printf("  Iterations  = %u\n\n", ITERATIONS);

    /* ── Test 1: Task Switch ── */
    uart_puts("[1/4] Task Switch...\n");
    stats_init(&sw_stats);
    sw_done = 0;
    xTaskCreate(vTaskSwB, "SwB", 512, NULL, 2, &hSwB);
    xTaskCreate(vTaskSwA, "SwA", 512, NULL, 2, &hSwA);
    while (!sw_done)
        vTaskDelay(pdMS_TO_TICKS(100));
    stats_print("Task Switch", &sw_stats);
    uart_putc('\n');

    /* ── Test 2: Preemption ── */
    uart_puts("[2/4] Preemption...\n");
    stats_init(&pre_stats);
    pre_done = 0;
    xTaskCreate(vTaskPreHigh, "PreH", 512, NULL, 3, &hPreHigh);
    xTaskCreate(vTaskPreLow, "PreL", 512, NULL, 2, NULL);
    while (!pre_done)
        vTaskDelay(pdMS_TO_TICKS(100));
    stats_print("Preemption", &pre_stats);
    uart_putc('\n');

    /* ── Test 3: IRQ Latency + Tick Jitter (passive, 5 seconds) ── */
    uart_puts("[3/4] IRQ Latency + Tick Jitter (5s)...\n");
    stats_init(&irq_stats);
    stats_init(&tick_stats);
    last_irq_time = 0;
    irq_collecting = 1;
    vTaskDelay(pdMS_TO_TICKS(5000));
    irq_collecting = 0;
    stats_print("IRQ Latency", &irq_stats);
    stats_print("Tick Delta", &tick_stats);
    uart_printf("  [Tick Delta] expected=%lu ns\n",
                (unsigned long)counts_to_ns(expected_interval));
    uart_putc('\n');

    /* ── Test 4: Semaphore Shuffle ── */
    uart_puts("[4/4] Semaphore Shuffle...\n");
    stats_init(&sem_stats);
    sem_done = 0;
    bench_sem = xSemaphoreCreateBinary();
    xTaskCreate(vTaskSemTaker, "SemT", 512, NULL, 3, NULL);
    xTaskCreate(vTaskSemGiver, "SemG", 512, NULL, 2, NULL);
    while (!sem_done)
        vTaskDelay(pdMS_TO_TICKS(100));
    stats_print("Sem Shuffle", &sem_stats);

    uart_puts("\n===== Done =====\n");
    for (;;)
        vTaskSuspend(NULL);
}

/* ── Hooks & stubs ── */

void vAssertCalled(const char *file, int line)
{
    uart_printf("[ASSERT] %s:%d\n", file, line);
    for (;;)
        __asm volatile ("wfi");
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    uart_printf("[FreeRTOS] Stack overflow: %s\n", pcTaskName);
    for (;;)
        __asm volatile ("wfi");
}

void *malloc(size_t size) { (void)size; return 0; }
void free(void *ptr) { (void)ptr; }

/* ── Entry ── */

int main(void)
{
    uart_init();
    uart_puts("\n[FreeRTOS] Rhealstone Benchmark Suite\n");

    cntfrq = read_cntfrq();
    timer_init();
    gic_init();

    xTaskCreate(vTaskControl, "Ctrl", 1024, NULL, 1, NULL);

    vTaskStartScheduler();
    for (;;)
        __asm volatile ("wfi");
}
