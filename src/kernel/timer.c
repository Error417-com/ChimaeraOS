/*
 * ChimaeraOS — PIT-based Millisecond Timer
 * src/kernel/timer.c
 *
 * Provides timer_ms() for elapsed-time measurements.
 *
 * Implementation
 * --------------
 * The PIT (Programmable Interval Timer) channel 0 is programmed by idt.c
 * to fire at ~100 Hz (every ~10 ms).  Each IRQ0 interrupt calls
 * timer_irq_tick() which increments g_ms_ticks by 10.
 *
 * timer_ms() returns g_ms_ticks — a monotonic millisecond counter that
 * wraps after ~49 days (uint32_t at 10ms resolution).
 *
 * This replaces the previous TSC-based implementation which wrapped after
 * only ~1.4 seconds on a 3 GHz host due to 32-bit TSC delta overflow.
 *
 * PIT channel 2 calibration (kept for timer_ticks_per_ms() compatibility)
 * -------------------------------------------------------------------------
 * PIT frequency: 1,193,182 Hz
 * 10 ms count:   11,932 (0x2E9C)
 */

#include "../include/timer.h"
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

/* ── TSC read (kept for timer_tsc() and calibration) ────────────────────── */

static inline void rdtsc_pair(uint32_t *lo, uint32_t *hi)
{
    __asm__ volatile ("rdtsc" : "=a"(*lo), "=d"(*hi));
}

/* ── PIT constants ───────────────────────────────────────────────────────── */

#define PIT_CH2_DATA   0x42
#define PIT_CMD        0x43
#define PIT_GATE_PORT  0x61

/* 10 ms at 1,193,182 Hz = 11,932 counts */
#define PIT_10MS_COUNT 11932u

/* ── Module state ────────────────────────────────────────────────────────── */

/* Monotonic millisecond counter — incremented by 10 on each PIT IRQ0 tick */
volatile uint32_t g_ms_ticks = 0;

/* TSC ticks per millisecond (from calibration, kept for compatibility) */
static uint32_t g_ticks_per_ms = 1;

/* ── timer_init ──────────────────────────────────────────────────────────── */

void timer_init(void)
{
    /*
     * Calibrate TSC against PIT channel 2 to determine ticks_per_ms.
     * This is kept for timer_ticks_per_ms() but is no longer used by
     * timer_ms() (which now uses g_ms_ticks instead).
     */
    uint8_t gate = inb(PIT_GATE_PORT);
    outb(PIT_GATE_PORT, gate & 0xFE);          /* gate off */

    /* Mode 0 (one-shot), binary, lo/hi byte, channel 2 */
    outb(PIT_CMD, 0xB0);
    outb(PIT_CH2_DATA, (uint8_t)(PIT_10MS_COUNT & 0xFF));
    outb(PIT_CH2_DATA, (uint8_t)(PIT_10MS_COUNT >> 8));

    /* Snapshot TSC and start the count */
    uint32_t start_lo, start_hi;
    rdtsc_pair(&start_lo, &start_hi);
    outb(PIT_GATE_PORT, (gate & 0xFE) | 0x01); /* gate on */

    /* Spin-wait until PIT output goes high (bit 5 of port 0x61) */
    while (!(inb(PIT_GATE_PORT) & 0x20)) {
        __asm__ volatile ("pause");
    }

    uint32_t end_lo, end_hi;
    rdtsc_pair(&end_lo, &end_hi);

    /* Gate off */
    outb(PIT_GATE_PORT, gate & 0xFE);

    uint32_t delta = end_lo - start_lo;
    g_ticks_per_ms = delta / 10u;
    if (g_ticks_per_ms == 0) g_ticks_per_ms = 1;

    /* Reset the millisecond counter */
    g_ms_ticks = 0;
}

/* ── timer_irq_tick ──────────────────────────────────────────────────────── */

/*
 * Called from the IRQ0 handler (idt.asm) on every PIT interrupt.
 * PIT is programmed at ~100 Hz (10 ms per tick), so we add 10 ms.
 */
void timer_irq_tick(void)
{
    g_ms_ticks += 10;
}

/* ── timer_ms ────────────────────────────────────────────────────────────── */

uint32_t timer_ms(void)
{
    return g_ms_ticks;
}

/* ── timer_tsc ───────────────────────────────────────────────────────────── */

uint64_t timer_tsc(void)
{
    uint32_t lo, hi;
    rdtsc_pair(&lo, &hi);
    return ((uint64_t)hi << 32) | lo;
}

/* ── timer_ticks_per_ms ──────────────────────────────────────────────────── */

uint32_t timer_ticks_per_ms(void)
{
    return g_ticks_per_ms;
}
