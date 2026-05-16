#ifndef VGA_H
#define VGA_H

#include "types.h"

void vga_init(void);
void vga_putchar(char c);
void vga_puts(const char *s);
void vga_clear(void);

#endif /* VGA_H */
