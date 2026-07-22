#include "FreeRTOS.h"
#include "task.h"
#include "gic.h"
#include "timer.h"
#include "uart.h"
#include "virtio_net.h"

extern void FreeRTOS_Tick_Handler(void);

static void idle_forever(void)
{
    for (;;)
        __asm volatile ("wfi");
}

static void system_off(void)
{
    register uint64_t x0 __asm("x0") = 0x84000008UL;
    __asm volatile ("hvc #0" : : "r"(x0) : "memory");
    idle_forever();
}

void vApplicationIRQHandler(uint32_t ulICCIAR)
{
    uint32_t irq = ulICCIAR & 0x3FF;

    if (virtio_net_handle_irq(irq))
        return;

    if (irq == 27)
        FreeRTOS_Tick_Handler();
}

static void vTaskVirtioNet(void *p)
{
    (void)p;

    uart_puts("\n===== FreeRTOS virtio-net demo =====\n");
    if (virtio_net_test()) {
        uart_puts("\n===== Received ok; shutting down =====\n");
        system_off();
    }
    uart_puts("\n===== No ok; staying alive =====\n");
    idle_forever();
}

void vAssertCalled(const char *file, int line)
{
    uart_printf("[ASSERT] %s:%d\n", file, line);
    idle_forever();
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    uart_printf("[FreeRTOS] Stack overflow: %s\n", pcTaskName);
    idle_forever();
}

void *malloc(size_t size) { (void)size; return 0; }
void free(void *ptr) { (void)ptr; }

int main(void)
{
    uart_init();
    uart_puts("\n[FreeRTOS] virtio-net demo\n");

    timer_init();
    gic_init();
    virtio_net_init();

    xTaskCreate(vTaskVirtioNet, "VNet", 1024, NULL, 1, NULL);

    vTaskStartScheduler();
    for (;;)
        __asm volatile ("wfi");
}
