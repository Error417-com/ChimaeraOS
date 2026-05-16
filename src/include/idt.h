/*
 * ChimaeraOS — IDT and PIC Initialisation
 * src/include/idt.h
 *
 * Provides:
 *   idt_init()   — build the 256-entry IDT, remap the 8259A PIC,
 *                  install the IRQ0 (PIT timer) handler, and load the IDT.
 *   idt_eoi(irq) — send End-Of-Interrupt to the PIC for the given IRQ line.
 *
 * After idt_init() returns, interrupts are still disabled.  The caller
 * must execute `sti` (or call sched_start()) to enable them.
 *
 * PIC remapping
 * -------------
 * The BIOS maps IRQ0–7 to vectors 0x08–0x0F, which collide with CPU
 * exception vectors.  We remap:
 *   Master PIC (IRQ 0–7)  → vectors 0x20–0x27
 *   Slave  PIC (IRQ 8–15) → vectors 0x28–0x2F
 *
 * Only IRQ0 (PIT timer, vector 0x20) is unmasked.  All other IRQs are
 * masked at the PIC.
 */

#ifndef IDT_H
#define IDT_H

#include "types.h"

/* Vector numbers after PIC remapping */
#define IRQ0_VECTOR   0x20   /* PIT timer */
#define IRQ1_VECTOR   0x21   /* keyboard  */

/*
 * idt_init() — set up the IDT and PIC.
 * Installs a default "spurious" handler for all 256 vectors, then
 * installs the real IRQ0 handler (defined in idt.asm).
 * Does NOT enable interrupts.
 */
void idt_init(void);

/*
 * idt_eoi(irq) — send EOI to the PIC.
 * irq is the IRQ line number (0–15), not the vector number.
 * For IRQ 8–15 (slave PIC), sends EOI to both slave and master.
 */
void idt_eoi(uint8_t irq);

#endif /* IDT_H */
