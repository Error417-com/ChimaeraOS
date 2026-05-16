/*
 * ChimaeraOS - Serial (UART) Driver
 * drivers/serial.c
 *
 * Drives COM1 (0x3F8) at 115200 baud for debug output.
 */
#include "../include/serial.h"
#include "../include/types.h"

#define COM1_BASE 0x3F8

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

void serial_init(void)
{
    outb(COM1_BASE + 1, 0x00);  /* Disable interrupts */
    outb(COM1_BASE + 3, 0x80);  /* Enable DLAB */
    outb(COM1_BASE + 0, 0x01);  /* Divisor low  (115200 baud) */
    outb(COM1_BASE + 1, 0x00);  /* Divisor high */
    outb(COM1_BASE + 3, 0x03);  /* 8N1, DLAB off */
    outb(COM1_BASE + 2, 0xC7);  /* FIFO, clear, 14-byte threshold */
    outb(COM1_BASE + 4, 0x0B);  /* IRQs enabled, RTS/DSR set */
}

static void serial_wait_tx(void)
{
    while (!(inb(COM1_BASE + 5) & 0x20)) {}
}

void serial_putchar(char c)
{
    serial_wait_tx();
    outb(COM1_BASE, (uint8_t)c);
}

void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') serial_putchar('\r');
        serial_putchar(*s++);
    }
}

void serial_hex8(uint8_t v)
{
    static const char h[] = "0123456789abcdef";
    serial_putchar(h[(v >> 4) & 0xF]);
    serial_putchar(h[ v       & 0xF]);
}

void serial_hex16(uint16_t v)
{
    serial_hex8((uint8_t)(v >> 8));
    serial_hex8((uint8_t)(v & 0xFF));
}

void serial_hex32(uint32_t v)
{
    serial_hex8((uint8_t)(v >> 24));
    serial_hex8((uint8_t)(v >> 16));
    serial_hex8((uint8_t)(v >>  8));
    serial_hex8((uint8_t)(v & 0xFF));
}

void serial_dec(uint32_t v)
{
    char buf[12];
    int  i = 0;
    if (v == 0) { serial_putchar('0'); return; }
    while (v > 0) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) serial_putchar(buf[--i]);
}
