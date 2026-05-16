#ifndef SERIAL_H
#define SERIAL_H

#include "types.h"

void serial_init(void);
void serial_putchar(char c);
void serial_puts(const char *s);
void serial_hex8(uint8_t v);
void serial_hex16(uint16_t v);
void serial_hex32(uint32_t v);
void serial_dec(uint32_t v);

#endif /* SERIAL_H */
