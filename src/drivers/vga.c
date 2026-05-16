/*
 * ChimaeraOS - VGA Text Mode Driver
 * drivers/vga.c
 */
#include "../include/vga.h"
#include "../include/types.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEM    ((uint16_t *)0xB8000)
#define VGA_COLOR  0x0F  /* white on black */

static int vga_col = 0;
static int vga_row = 0;

void vga_clear(void)
{
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA_MEM[i] = (uint16_t)(VGA_COLOR << 8) | ' ';
    vga_col = 0;
    vga_row = 0;
}

static void vga_scroll(void)
{
    for (int r = 1; r < VGA_HEIGHT; r++)
        for (int c = 0; c < VGA_WIDTH; c++)
            VGA_MEM[(r-1)*VGA_WIDTH + c] = VGA_MEM[r*VGA_WIDTH + c];
    for (int c = 0; c < VGA_WIDTH; c++)
        VGA_MEM[(VGA_HEIGHT-1)*VGA_WIDTH + c] = (uint16_t)(VGA_COLOR << 8) | ' ';
    vga_row = VGA_HEIGHT - 1;
}

void vga_init(void)
{
    vga_clear();
}

void vga_putchar(char c)
{
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
    } else if (c == '\r') {
        vga_col = 0;
    } else {
        VGA_MEM[vga_row * VGA_WIDTH + vga_col] =
            (uint16_t)(VGA_COLOR << 8) | (uint8_t)c;
        vga_col++;
        if (vga_col >= VGA_WIDTH) {
            vga_col = 0;
            vga_row++;
        }
    }
    if (vga_row >= VGA_HEIGHT) vga_scroll();
}

void vga_puts(const char *s)
{
    while (*s) vga_putchar(*s++);
}
