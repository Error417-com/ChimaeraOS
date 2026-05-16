/*
 * ChimaeraOS — PIT-based Millisecond Timer
 * src/include/timer.h
 *
 * Provides a monotonic millisecond clock driven by PIT IRQ0 ticks.
 * Call timer_init() once early in kernel_main(); thereafter timer_ms()
 * returns milliseconds elapsed since init.
 *
 * Implementation notes
 * --------------------
 * The PIT channel 0 is programmed at ~100 Hz (10 ms per tick) by idt.c.
 * Each IRQ0 interrupt calls timer_irq_tick() which increments g_ms_ticks
 * by 10.  timer_ms() returns g_ms_ticks.
 *
 * Accuracy: ±10 ms (one PIT tick resolution).
 * Overflow: wraps after ~497 days (uint32_t at 10ms resolution).
 */
#ifndef TIMER_H
#define TIMER_H

#include "types.h"

/*
 * timer_init()
 * Initialise the timer subsystem.  Calibrates TSC against PIT channel 2
 * (for timer_ticks_per_ms()) and resets g_ms_ticks to 0.
 * Must be called once before any other timer function.
 */
void timer_init(void);

/*
 * timer_irq_tick()
 * Called from the IRQ0 handler on every PIT tick (~100 Hz).
 * Increments the monotonic millisecond counter by 10.
 */
void timer_irq_tick(void);

/*
 * timer_ms()
 * Return milliseconds elapsed since timer_init() was called.
 * Resolution: 10 ms (one PIT tick).
 * Wraps after ~497 days.
 */
uint32_t timer_ms(void);

/*
 * timer_tsc()
 * Return the raw 64-bit TSC value.  Useful for sub-millisecond intervals.
 */
uint64_t timer_tsc(void);

/*
 * timer_ticks_per_ms()
 * Return the calibrated TSC ticks per millisecond (from PIT calibration).
 */
uint32_t timer_ticks_per_ms(void);

#endif /* TIMER_H */
