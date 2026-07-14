/*
 * freertos.lds — Linker script for FreeRTOS guest on kvmm
 *
 * The VMM loads the flat binary at mem_base + 0x00800000 = 0x70800000.
 */

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x70800000, LENGTH = 16M
}

SECTIONS
{
    .text : {
        *(.text.boot)
        *(.text*)
    } > RAM

    .rodata : {
        *(.rodata*)
    } > RAM

    .data : {
        *(.data*)
    } > RAM

    . = ALIGN(16);
    __bss_start = .;
    .bss : {
        *(.bss*)
        *(COMMON)
    } > RAM
    __bss_end = .;

    . = ALIGN(16);
    _heap_start = .;
    . = . + 128K;
    _heap_end = .;

    . = ALIGN(16);
    _stack_bottom = .;
    . = . + 16K;
    _stack_top = .;
}
