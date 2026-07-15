#include "FreeRTOS.h"
#include "task.h"
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

void vApplicationIRQHandler(uint32_t ulICCIAR)
{
    uint32_t irq = ulICCIAR & 0x3FF;

    if (irq == 27) {
        FreeRTOS_Tick_Handler();
    }
}

static volatile uint64_t tick_min = UINT64_MAX;
static volatile uint64_t tick_max;
static volatile uint64_t tick_sum;
static volatile uint32_t tick_count;

void vApplicationTickHook(void)
{
    static uint64_t last;
    uint64_t now = read_cntvct();

    if (last == 0) {
        last = now;
        return;
    }

    uint64_t delta = now - last;
    last = now;
    tick_count++;

    if (delta < tick_min) tick_min = delta;
    if (delta > tick_max) tick_max = delta;
    tick_sum += delta;

    if (tick_count <= 10) {
        uart_printf("[tick %u] delta=%lu\n", tick_count, (unsigned long)delta);
    }
}

static void vTaskReport(void *pvParameters)
{
    (void)pvParameters;
    uint64_t expected = cntfrq / configTICK_RATE_HZ;

    uart_printf("[timer] expected delta = %lu counts (%lu us)\n",
                (unsigned long)expected,
                (unsigned long)(expected * 1000000ULL / cntfrq));

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        uint32_t n = tick_count;
        if (n == 0) continue;

        uint64_t avg = tick_sum / n;
        int64_t avg_err = (int64_t)avg - (int64_t)expected;
        int64_t min_err = (int64_t)tick_min - (int64_t)expected;
        int64_t max_err = (int64_t)tick_max - (int64_t)expected;
        uint64_t jitter = tick_max - tick_min;

        uart_printf("[timer] n=%u  avg=%lu(%ld)  min=%lu(%ld)  max=%lu(%ld)  "
                    "jitter=%lu  jitter_us=%lu\n",
                    n,
                    (unsigned long)avg, (long)avg_err,
                    (unsigned long)tick_min, (long)min_err,
                    (unsigned long)tick_max, (long)max_err,
                    (unsigned long)jitter,
                    (unsigned long)(jitter * 1000000ULL / cntfrq));
    }
}

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

int main(void)
{
    uart_init();
    uart_puts("\n[FreeRTOS] Timer precision test\n");

    cntfrq = read_cntfrq();
    uart_printf("[FreeRTOS] CNTFRQ = %lu Hz\n", (unsigned long)cntfrq);

    timer_init();
    gic_init();

    xTaskCreate(vTaskReport, "Report", 1024, NULL, 2, NULL);

    uart_puts("[FreeRTOS] Starting scheduler...\n");
    vTaskStartScheduler();

    for (;;)
        __asm volatile ("wfi");
}
