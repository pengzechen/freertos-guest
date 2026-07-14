#include "FreeRTOS.h"
#include "task.h"
#include "gic.h"
#include "timer.h"
#include "uart.h"

extern void FreeRTOS_Tick_Handler(void);

void vApplicationIRQHandler(uint32_t ulICCIAR)
{
    uint32_t irq = ulICCIAR & 0x3FF;

    if (irq == 27) {
        FreeRTOS_Tick_Handler();
    }
}

static void vTaskDemo(void *pvParameters)
{
    uint32_t id = (uint32_t)(uintptr_t)pvParameters;
    uint32_t count = 0;

    for (;;) {
        uart_printf("[FreeRTOS] Task %u: count=%u tick=%lu\n",
                    id, count, (unsigned long)xTaskGetTickCount());
        count++;
        vTaskDelay(pdMS_TO_TICKS(1000));
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

    timer_init();
    gic_init();

    uart_puts("[FreeRTOS] Creating tasks...\n");

    xTaskCreate(vTaskDemo, "Task0", 512, (void *)0, 2, NULL);
    xTaskCreate(vTaskDemo, "Task1", 512, (void *)1, 2, NULL);
    xTaskCreate(vTaskDemo, "Task2", 512, (void *)2, 2, NULL);
    xTaskCreate(vTaskDemo, "Task3", 512, (void *)3, 2, NULL);

    uart_puts("[FreeRTOS] Starting scheduler...\n");
    vTaskStartScheduler();

    uart_puts("[FreeRTOS] ERROR: scheduler returned!\n");
    for (;;)
        __asm volatile ("wfi");
}
