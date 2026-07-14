/* FreeRTOS heap_4 implementation needs these */
#include "FreeRTOS.h"

extern char _heap_start[];
extern char _heap_end[];

static uint8_t ucHeap[ configTOTAL_HEAP_SIZE ] __attribute__((aligned(16)));
