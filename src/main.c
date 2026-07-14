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

static inline uint64_t counts_to_us(uint64_t counts)
{
    return counts * 1000000ULL / cntfrq;
}

void vApplicationIRQHandler(uint32_t ulICCIAR)
{
    uint32_t irq = ulICCIAR & 0x3FF;

    if (irq == 27) {
        FreeRTOS_Tick_Handler();
    }
}

static void vTaskBenchmark(void *pvParameters)
{
    uint32_t id = (uint32_t)(uintptr_t)pvParameters;
    const TickType_t delay_ticks = pdMS_TO_TICKS(1000);
    const uint64_t expected_us = 1000000ULL / configTICK_RATE_HZ * delay_ticks;

    uint64_t prev = read_cntvct();
    uint64_t min_us = UINT64_MAX, max_us = 0, sum_us = 0;
    uint32_t count = 0;

    for (;;) {
        vTaskDelay(delay_ticks);
        count++;

        uint64_t now = read_cntvct();
        uint64_t elapsed_us = counts_to_us(now - prev);
        prev = now;

        if (elapsed_us < min_us) min_us = elapsed_us;
        if (elapsed_us > max_us) max_us = elapsed_us;
        sum_us += elapsed_us;

        int64_t drift_us = (int64_t)elapsed_us - (int64_t)expected_us;

        if (count <= 5 || count % 50 == 0) {
            uint64_t avg_us = sum_us / count;
            int64_t avg_drift = (int64_t)avg_us - (int64_t)expected_us;
            uart_printf("[Task %u] #%u  actual=%lu us  drift=%ld us  "
                        "min=%lu max=%lu avg=%lu avg_drift=%ld\n",
                        id, count,
                        (unsigned long)elapsed_us,
                        (long)drift_us,
                        (unsigned long)min_us,
                        (unsigned long)max_us,
                        (unsigned long)avg_us,
                        (long)avg_drift);
        }
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
    uart_puts("\n[FreeRTOS] Booting on kvmm (AArch64 EL1)\n");

    cntfrq = read_cntfrq();
    uart_printf("[FreeRTOS] CNTFRQ = %lu Hz\n", (unsigned long)cntfrq);

    timer_init();
    gic_init();

    uart_printf("[FreeRTOS] Tick rate = %u Hz, delay = %u ticks (expect %lu us)\n",
                configTICK_RATE_HZ,
                (unsigned)pdMS_TO_TICKS(1000),
                (unsigned long)(1000000ULL / configTICK_RATE_HZ * pdMS_TO_TICKS(1000)));

    xTaskCreate(vTaskBenchmark, "Bench0", 1024, (void *)0, 2, NULL);
    xTaskCreate(vTaskBenchmark, "Bench1", 1024, (void *)1, 2, NULL);

    uart_puts("[FreeRTOS] Starting scheduler...\n");
    vTaskStartScheduler();

    uart_puts("[FreeRTOS] ERROR: scheduler returned!\n");
    for (;;)
        __asm volatile ("wfi");
}
