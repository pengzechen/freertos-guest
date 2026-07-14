/*
 * startup.S — EL1 entry for FreeRTOS guest on kvmm
 *
 * The VMM enters us at EL1h with MMU off, DAIF masked, x0 = DTB address.
 * We set up a stack, clear BSS, and call main().
 */

    .section .text.boot
    .global _start
    .type _start, @function

_start:
    /* We are at EL1h. Set up the stack. */
    ldr     x1, =_stack_top
    mov     sp, x1

    /* Clear BSS */
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
1:  cmp     x1, x2
    b.ge    2f
    str     xzr, [x1], #8
    b       1b
2:
    /* Call main */
    bl      main

    /* Should not return */
3:  wfi
    b       3b

    .size _start, . - _start
