/*
 * ChimaeraOS — IDT and PIC Initialisation
 * src/kernel/idt.c
 *
 * Builds the 256-entry Interrupt Descriptor Table, remaps the 8259A PIC
 * so IRQ0–7 map to vectors 0x20–0x27 (avoiding collision with CPU exceptions),
 * programs PIT channel 0 to fire at ~100 Hz, installs the IRQ0 handler,
 * and loads the IDT via LIDT.
 *
 * After idt_init() returns, interrupts are still disabled.  The caller
 * must execute STI (or call sched_start()) to enable them.
 *
 * PIC remapping
 * -------------
 * Master PIC: IRQ0–7  → vectors 0x20–0x27
 * Slave  PIC: IRQ8–15 → vectors 0x28–0x2F
 *
 * Only IRQ0 is unmasked.  All other IRQs are masked at the PIC.
 *
 * PIT channel 0
 * -------------
 * Frequency: 1,193,182 Hz
 * Divisor for ~100 Hz: 11,932 (0x2E9C) → ~10 ms per tick
 * Mode: mode 3 (square wave), binary, lo/hi byte, channel 0
 */

#include "../include/idt.h"
#include "../include/types.h"

/* ── Port I/O helpers ────────────────────────────────────────────────────── */

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

static inline void io_wait(void)
{
    /* Write to an unused port to introduce a small I/O delay */
    outb(0x80, 0x00);
}

/* ── IDT entry format (32-bit protected mode, interrupt gate) ────────────── */

typedef struct __attribute__((packed)) {
    uint16_t offset_lo;     /* bits 0–15 of handler address  */
    uint16_t selector;      /* code segment selector (0x08)  */
    uint8_t  zero;          /* always 0                      */
    uint8_t  type_attr;     /* type + DPL + present bit      */
    uint16_t offset_hi;     /* bits 16–31 of handler address */
} idt_entry_t;

/* type_attr for a 32-bit interrupt gate, ring-0, present */
#define IDT_INT_GATE_RING0  0x8E

/* ── IDTR structure ──────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint16_t limit;         /* size of IDT in bytes - 1 */
    uint32_t base;          /* linear address of IDT    */
} idtr_t;

/* ── IDT storage ─────────────────────────────────────────────────────────── */

static idt_entry_t g_idt[256];
static idtr_t      g_idtr;

/* ── External symbols from idt.asm ──────────────────────────────────────── */

extern void idt_load(void *idtr_ptr);
extern void irq0_handler(void);
extern void irq_spurious_handler(void);
extern void yield_int_handler(void);

/* ── Helper: install one IDT entry ──────────────────────────────────────── */

static void idt_set_gate(uint8_t vector, void (*handler)(void),
                         uint16_t selector, uint8_t type_attr)
{
    uint32_t addr = (uint32_t)handler;
    g_idt[vector].offset_lo = (uint16_t)(addr & 0xFFFF);
    g_idt[vector].selector  = selector;
    g_idt[vector].zero      = 0;
    g_idt[vector].type_attr = type_attr;
    g_idt[vector].offset_hi = (uint16_t)((addr >> 16) & 0xFFFF);
}

/* ── PIC constants ───────────────────────────────────────────────────────── */

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

#define PIC_EOI    0x20

/* Initialisation Control Words */
#define ICW1_INIT  0x10
#define ICW1_ICW4  0x01
#define ICW4_8086  0x01

/* ── PIT constants ───────────────────────────────────────────────────────── */

#define PIT_CH0_DATA  0x40
#define PIT_CMD_PORT  0x43

/* Mode 3 (square wave), binary, lo/hi byte, channel 0 */
#define PIT_CMD_CH0_MODE3  0x36

/* Divisor for ~100 Hz: 1,193,182 / 100 = 11,931 ≈ 11,932 */
#define PIT_DIVISOR  11932u

/* ── pic_remap ───────────────────────────────────────────────────────────── */

static void pic_remap(uint8_t master_offset, uint8_t slave_offset)
{
    /* Save existing masks */
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    /* Start initialisation sequence (cascade mode) */
    outb(PIC1_CMD,  ICW1_INIT | ICW1_ICW4);  io_wait();
    outb(PIC2_CMD,  ICW1_INIT | ICW1_ICW4);  io_wait();

    /* ICW2: vector offsets */
    outb(PIC1_DATA, master_offset);           io_wait();
    outb(PIC2_DATA, slave_offset);            io_wait();

    /* ICW3: cascade wiring */
    outb(PIC1_DATA, 0x04);                    io_wait(); /* slave on IRQ2 */
    outb(PIC2_DATA, 0x02);                    io_wait(); /* slave ID = 2  */

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, ICW4_8086);               io_wait();
    outb(PIC2_DATA, ICW4_8086);               io_wait();

    /* Restore masks */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

/* ── pit_init ────────────────────────────────────────────────────────────── */

static void pit_init(uint16_t divisor)
{
    /* Mode 3 (square wave), binary, lo/hi byte, channel 0 */
    outb(PIT_CMD_PORT,  PIT_CMD_CH0_MODE3);
    outb(PIT_CH0_DATA,  (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0_DATA,  (uint8_t)(divisor >> 8));
}

/* ── idt_init ────────────────────────────────────────────────────────────── */

void idt_init(void)
{
    /* Install the spurious handler for all 256 vectors.
     * GRUB sets CS=0x10 (kernel code segment selector). */
    for (uint32_t i = 0; i < 256; i++) {
        idt_set_gate((uint8_t)i, irq_spurious_handler, 0x10,
                     IDT_INT_GATE_RING0);
    }

    /* Remap PIC: master → 0x20, slave → 0x28 */
    pic_remap(0x20, 0x28);

    /* Mask ALL IRQs on both PICs */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    /* Install the real IRQ0 handler at vector 0x20 */
    idt_set_gate(IRQ0_VECTOR, irq0_handler, 0x10, IDT_INT_GATE_RING0);

    /* Install the cooperative yield handler at vector 0x30 (software int) */
    idt_set_gate(0x30, yield_int_handler, 0x10, IDT_INT_GATE_RING0);

    /* Unmask only IRQ0 on the master PIC */
    outb(PIC1_DATA, 0xFE);   /* bit 0 = IRQ0, 0 = unmasked */

    /* Program PIT channel 0 to ~100 Hz */
    pit_init(PIT_DIVISOR);

    /* Build the IDTR and load it */
    g_idtr.limit = (uint16_t)(sizeof(g_idt) - 1);
    g_idtr.base  = (uint32_t)g_idt;
    idt_load(&g_idtr);
}

/* ── idt_eoi ─────────────────────────────────────────────────────────────── */

void idt_eoi(uint8_t irq)
{
    if (irq >= 8) {
        outb(PIC2_CMD, PIC_EOI);   /* slave PIC */
    }
    outb(PIC1_CMD, PIC_EOI);       /* master PIC */
}
