/*
 * ChimaeraOS — Scheduler Demo Kernel
 * src/kernel/kernel_sched.c
 *
 * Compiled with -DSCHED_DEMO.
 *
 * Demonstrates the preemptive round-robin scheduler:
 *   - Two tasks (task_a and task_b) print to the serial port alternately.
 *   - Neither task calls yield() explicitly.
 *   - The PIT IRQ0 handler fires every ~10 ms and preempts the running task
 *     after SCHED_QUANTUM_TICKS (5) ticks = ~50 ms quantum.
 *
 * Expected serial output pattern (interleaved):
 *   [TASK A] tick 1
 *   [TASK B] tick 1
 *   [TASK A] tick 2
 *   [TASK B] tick 2
 *   ...
 *   [SCHED] All tasks complete
 *   [SCHED_DEMO] PASS
 *
 * Integration test assertions (checked by sched_test.py):
 *   S1  "[TASK A]" appears at least 5 times
 *   S2  "[TASK B]" appears at least 5 times
 *   S3  "[TASK A]" and "[TASK B]" lines alternate (no long run of one task)
 *   S4  "[SCHED] All tasks complete" appears
 *   S5  "[SCHED_DEMO] PASS" appears
 *   S6  No "[SCHED_DEMO] FAIL" lines
 *   S7  No PANIC in serial log
 */

#include "../include/serial.h"
#include "../include/vga.h"
#include "../include/mm.h"
#include "../include/idt.h"
#include "../include/sched.h"
#include "../include/timer.h"
#include "../include/panic.h"
#include "../include/types.h"

/* ── Helpers ──────────────────────────────────────────────────────────────── */

/* Print a decimal integer to serial */
static void serial_uint(uint32_t n)
{
    if (n == 0) { serial_putchar('0'); return; }
    char buf[12];
    int i = 0;
    while (n) { buf[i++] = '0' + (int)(n % 10); n /= 10; }
    for (int j = i - 1; j >= 0; j--) serial_putchar(buf[j]);
}

/* ── Task A ───────────────────────────────────────────────────────────────── */

#define TASK_ITERATIONS  10

static void task_a(void)
{
    for (uint32_t i = 1; i <= TASK_ITERATIONS; i++) {
        serial_puts("[TASK A] tick ");
        serial_uint(i);
        serial_puts("\n");

        /* Busy-wait for ~200 ms so the timer has time to preempt us.
         * We do NOT call yield() — preemption must happen via IRQ0. */
        uint32_t t0 = timer_ms();
        while (timer_ms() - t0 < 200) {
            __asm__ volatile ("pause");
        }
    }
    serial_puts("[TASK A] done\n");
}

/* ── Task B ───────────────────────────────────────────────────────────────── */

static void task_b(void)
{
    for (uint32_t i = 1; i <= TASK_ITERATIONS; i++) {
        serial_puts("[TASK B] tick ");
        serial_uint(i);
        serial_puts("\n");

        uint32_t t0 = timer_ms();
        while (timer_ms() - t0 < 200) {
            __asm__ volatile ("pause");
        }
    }
    serial_puts("[TASK B] done\n");
}

/* ── kernel_main ──────────────────────────────────────────────────────────── */

void kernel_main(void)
{
    serial_init();
    vga_init();
    mm_init();
    symtab_init();
    timer_init();

    serial_puts("[SCHED_DEMO] Scheduler demo kernel starting\n");

    /* Initialise the IDT and PIC — this programs the PIT to ~100 Hz
     * and installs the IRQ0 handler.  Interrupts are still disabled. */
    idt_init();
    serial_puts("[SCHED_DEMO] IDT and PIC initialised\n");

    /* Initialise the scheduler — registers the current context as task 0 */
    sched_init();

    /* Spawn the two demo tasks */
    int id_a = sched_spawn(task_a, "task_a");
    int id_b = sched_spawn(task_b, "task_b");

    if (id_a < 0 || id_b < 0) {
        serial_puts("[SCHED_DEMO] FAIL: sched_spawn failed\n");
        panic("sched_spawn failed");
    }

    serial_puts("[SCHED_DEMO] Tasks spawned: A=");
    serial_putchar('0' + (char)id_a);
    serial_puts(" B=");
    serial_putchar('0' + (char)id_b);
    serial_puts("\n");

    serial_puts("[SCHED_DEMO] Starting scheduler (no explicit yield in tasks)\n");

    /* sched_start() enables interrupts and spins until all tasks are done */
    sched_start();

    serial_puts("[SCHED_DEMO] PASS\n");

    for (;;) __asm__("hlt");
}
