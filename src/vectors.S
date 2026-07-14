/*
 * vectors.S — EL1 exception vector table for FreeRTOS guest
 *
 * AArch64 vector table: 16 entries × 128 bytes each, aligned to 2KB.
 * Each entry has exactly 32 instructions (128 bytes).
 *
 * FreeRTOS tasks run with SPSEL=0 (SP_EL0), so task exceptions
 * are "current EL with SP_EL0" (offset 0x000-0x180).
 * IRQ handler switches to SPSEL=1 (SP_EL1), so nested exceptions
 * while in the handler are "current EL with SP_ELx" (offset 0x200-0x380).
 */

    .extern FreeRTOS_IRQ_Handler
    .extern FreeRTOS_SWI_Handler

    .section .text
    .global _freertos_vector_table
    .balign 2048

_freertos_vector_table:

/* --- Current EL with SP_EL0 (EL1t) --- */

/* 0x000: Synchronous (SVC from task) */
.balign 128
    b       FreeRTOS_SWI_Handler

/* 0x080: IRQ (timer / device interrupt from task context) */
.balign 128
    b       FreeRTOS_IRQ_Handler

/* 0x100: FIQ */
.balign 128
    b       _unhandled

/* 0x180: SError */
.balign 128
    b       _unhandled

/* --- Current EL with SP_ELx (EL1h) --- */

/* 0x200: Synchronous */
.balign 128
    b       FreeRTOS_SWI_Handler

/* 0x280: IRQ (nested IRQ while in handler) */
.balign 128
    b       FreeRTOS_IRQ_Handler

/* 0x300: FIQ */
.balign 128
    b       _unhandled

/* 0x380: SError */
.balign 128
    b       _unhandled

/* --- Lower EL using AArch64 --- */

/* 0x400: Synchronous */
.balign 128
    b       _unhandled

/* 0x480: IRQ */
.balign 128
    b       _unhandled

/* 0x500: FIQ */
.balign 128
    b       _unhandled

/* 0x580: SError */
.balign 128
    b       _unhandled

/* --- Lower EL using AArch32 --- */

/* 0x600: Synchronous */
.balign 128
    b       _unhandled

/* 0x680: IRQ */
.balign 128
    b       _unhandled

/* 0x700: FIQ */
.balign 128
    b       _unhandled

/* 0x780: SError */
.balign 128
    b       _unhandled

_unhandled:
    b       .
