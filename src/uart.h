#ifndef UART_H
#define UART_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_printf(const char *fmt, ...);

#endif
