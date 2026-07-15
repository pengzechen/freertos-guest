#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#define configUSE_PREEMPTION                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  0
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     1
#define configTICK_RATE_HZ                      100
#define configMAX_PRIORITIES                    5
#define configMINIMAL_STACK_SIZE                256
#define configTOTAL_HEAP_SIZE                   ( 64 * 1024 )
#define configMAX_TASK_NAME_LEN                 16
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           0
#define configQUEUE_REGISTRY_SIZE               0
#define configUSE_QUEUE_SETS                     0
#define configUSE_TIME_SLICING                  1
#define configSTACK_DEPTH_TYPE                  uint32_t
#define configMESSAGE_BUFFER_LENGTH_TYPE        size_t

#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1

#define configUSE_TASK_FPU_SUPPORT              1
#define configNUMBER_OF_CORES                   1

/* GICv2 addresses for QEMU virt / kvmm */
#define configINTERRUPT_CONTROLLER_BASE_ADDRESS         0x08000000UL
#define configINTERRUPT_CONTROLLER_CPU_INTERFACE_OFFSET  0x00010000UL
#define configUNIQUE_INTERRUPT_PRIORITIES                32
#define configMAX_API_CALL_INTERRUPT_PRIORITY            18

/* Timer setup / clear — implemented in timer.c */
void vSetupTickInterrupt(void);
void vClearTickInterrupt(void);
#define configSETUP_TICK_INTERRUPT()    vSetupTickInterrupt()
#define configCLEAR_TICK_INTERRUPT()    vClearTickInterrupt()

/* Hook functions */
#define configUSE_MALLOC_FAILED_HOOK    0
#define configCHECK_FOR_STACK_OVERFLOW  0

/* Co-routine definitions (unused) */
#define configUSE_CO_ROUTINES           0

/* Software timer definitions */
#define configUSE_TIMERS                1
#define configTIMER_TASK_PRIORITY       3
#define configTIMER_QUEUE_LENGTH        5
#define configTIMER_TASK_STACK_DEPTH    configMINIMAL_STACK_SIZE

/* Assert */
void vAssertCalled(const char *file, int line);
#define configASSERT( x ) if( !( x ) ) vAssertCalled( __FILE__, __LINE__ )

/* Include optional API functions */
#define INCLUDE_vTaskDelay                  1
#define INCLUDE_vTaskDelayUntil             0
#define INCLUDE_vTaskDelete                 0
#define INCLUDE_xTaskGetSchedulerState      0
#define INCLUDE_uxTaskPriorityGet           0
#define INCLUDE_vTaskPrioritySet            0
#define INCLUDE_vTaskSuspend                1

#endif /* FREERTOS_CONFIG_H */
