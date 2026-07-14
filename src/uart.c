#include "uart.h"

#define PL011_BASE  0x09000000UL
#define UARTDR      (*(volatile uint32_t *)(PL011_BASE + 0x000))
#define UARTFR      (*(volatile uint32_t *)(PL011_BASE + 0x018))
#define UARTFR_TXFF (1 << 5)

void uart_init(void)
{
    /* PL011 is already configured by the VMM / hardware. */
}

void uart_putc(char c)
{
    while (UARTFR & UARTFR_TXFF)
        ;
    UARTDR = (uint32_t)c;
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

static void print_unsigned(uint64_t val, int base, int width)
{
    char buf[20];
    int i = 0;
    if (val == 0)
        buf[i++] = '0';
    while (val) {
        int d = val % base;
        buf[i++] = d < 10 ? '0' + d : 'a' + d - 10;
        val /= base;
    }
    while (i < width)
        buf[i++] = '0';
    while (i--)
        uart_putc(buf[i]);
}

void uart_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            if (*fmt == '\n')
                uart_putc('\r');
            uart_putc(*fmt);
            continue;
        }
        fmt++;
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        switch (*fmt) {
        case 'u':
            print_unsigned(va_arg(ap, unsigned int), 10, width);
            break;
        case 'l':
            fmt++;
            if (*fmt == 'u')
                print_unsigned(va_arg(ap, uint64_t), 10, width);
            break;
        case 'x':
            print_unsigned(va_arg(ap, unsigned int), 16, width);
            break;
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) { uart_putc('-'); v = -v; }
            print_unsigned((unsigned)v, 10, width);
            break;
        }
        case 's':
            uart_puts(va_arg(ap, const char *));
            break;
        case '%':
            uart_putc('%');
            break;
        default:
            uart_putc('%');
            uart_putc(*fmt);
            break;
        }
    }
    va_end(ap);
}
