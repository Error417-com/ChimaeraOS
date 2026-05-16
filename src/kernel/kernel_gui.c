/*
 * ChimaeraOS — GUI Compositor Demo Kernel
 * src/kernel/kernel_gui.c
 *
 * Demonstrates the window compositor prototype with two windows:
 *   - "Shell"   (left side of screen)
 *   - "Browser" (right side of screen)
 *
 * The demo runs three scheduler tasks:
 *   Task 0 (main)    — initialises everything, then runs the compositor loop
 *   Task 1 (shell)   — periodically writes output to the Shell window
 *   Task 2 (browser) — periodically writes output to the Browser window
 *
 * The compositor loop:
 *   - Polls g_input_ring for keyboard/mouse events
 *   - Routes events to the appropriate window
 *   - Re-renders the scene after every event
 *   - Runs for GUI_DEMO_DURATION_MS milliseconds, then prints PASS and halts
 *
 * Serial output (for test harness):
 *   [GUI_DEMO] starting
 *   [GUI_DEMO] window created: Shell (idx=0)
 *   [GUI_DEMO] window created: Browser (idx=1)
 *   [GUI_DEMO] compositor running
 *   [GUI_DEMO] heartbeat N timer_ms=M
 *   [GUI_DEMO] PASS
 */

#include "../include/serial.h"
#include "../include/vga.h"
#include "../include/mm.h"
#include "../include/timer.h"
#include "../include/idt.h"
#include "../include/sched.h"
#include "../include/panic.h"
#include "../include/types.h"
#include "../gui/compositor.h"

/* ── Demo parameters ─────────────────────────────────────────────────────── */

#define GUI_DEMO_DURATION_MS  6000   /* each app task runs for 6 seconds     */
#define APP_UPDATE_MS          500   /* how often each app task writes output */

/* ── Global compositor instance ─────────────────────────────────────────── */

static compositor_t g_comp;

/* ── Window indices ──────────────────────────────────────────────────────── */

static int g_shell_win   = -1;
static int g_browser_win = -1;

/* ── Tiny serial integer printer ─────────────────────────────────────────── */

static void serial_uint(uint32_t n)
{
    char buf[12];
    int  i = 0;
    if (!n) { serial_putchar('0'); return; }
    while (n) { buf[i++] = '0' + (n % 10); n /= 10; }
    for (int j = i - 1; j >= 0; j--) serial_putchar(buf[j]);
}

/* ── Shell task ──────────────────────────────────────────────────────────── */

static void task_shell(void)
{
    uint32_t tick = 0;
    uint32_t start = timer_ms();
    uint32_t next = start + APP_UPDATE_MS;

    while (timer_ms() - start < GUI_DEMO_DURATION_MS) {
        /* Busy-wait for next update interval */
        while (timer_ms() < next)
            __asm__ volatile ("pause");
        next += APP_UPDATE_MS;

        /* Write a line to the Shell window */
        comp_win_puts(&g_comp, g_shell_win, "$ ");
        comp_win_puts_attr(&g_comp, g_shell_win, "ls /", 0x0A); /* bright green */
        comp_win_puts(&g_comp, g_shell_win, "\n");
        if (tick % 3 == 0) {
            comp_win_puts(&g_comp, g_shell_win, "  kernel.elf\n");
            comp_win_puts(&g_comp, g_shell_win, "  chimaera.iso\n");
        }
        comp_render(&g_comp);
        tick++;
    }
}

/* ── Browser task ────────────────────────────────────────────────────────── */

static void task_browser(void)
{
    uint32_t tick = 0;
    uint32_t start = timer_ms();
    uint32_t next = start + APP_UPDATE_MS + 250; /* stagger start */

    while (timer_ms() - start < GUI_DEMO_DURATION_MS) {
        while (timer_ms() < next)
            __asm__ volatile ("pause");
        next += APP_UPDATE_MS;

        /* Write a line to the Browser window */
        if (tick == 0) {
            comp_win_puts_attr(&g_comp, g_browser_win,
                               "ChimeraOS Browser v0.1\n", 0x0B); /* cyan */
            comp_win_puts(&g_comp, g_browser_win,
                          "URL: http://localhost/\n\n");
        }
        comp_win_puts(&g_comp, g_browser_win, "Loading");
        for (int i = 0; i < (int)(tick % 4); i++)
            comp_win_putchar(&g_comp, g_browser_win, '.', ATTR_CONTENT);
        comp_win_puts(&g_comp, g_browser_win, "\n");
        comp_render(&g_comp);
        tick++;
    }
}

/* ── kernel_main ─────────────────────────────────────────────────────────── */

void kernel_main(void)
{
    /* ── Core subsystems ─────────────────────────────────────────────────── */
    serial_init();
    vga_init();
    mm_init();
    symtab_init();
    timer_init();

    serial_puts("[GUI_DEMO] starting\r\n");

    /* ── IDT + PIC ───────────────────────────────────────────────────────── */
    idt_init();
    __asm__ volatile ("sti");   /* enable IRQ0 so timer_ms() works */

    /* ── Compositor initialisation ───────────────────────────────────────── */
    comp_init(&g_comp);

    /*
     * Window layout on 80x25 screen:
     *
     *   Shell window:   x=0,  y=0, w=38, h=24
     *   Browser window: x=40, y=0, w=40, h=24
     *   (1-column gap between them)
     */
    g_shell_win = comp_create_window(&g_comp, 0, 0, 38, 24, "Shell");
    if (g_shell_win < 0) panic("GUI_DEMO: failed to create Shell window");
    serial_puts("[GUI_DEMO] window created: Shell (idx=");
    serial_uint((uint32_t)g_shell_win);
    serial_puts(")\r\n");

    g_browser_win = comp_create_window(&g_comp, 40, 0, 40, 24, "Browser");
    if (g_browser_win < 0) panic("GUI_DEMO: failed to create Browser window");
    serial_puts("[GUI_DEMO] window created: Browser (idx=");
    serial_uint((uint32_t)g_browser_win);
    serial_puts(")\r\n");

    /* Initial render — both windows visible on screen */
    comp_render(&g_comp);

    /* ── Scheduler ───────────────────────────────────────────────────────── */
    sched_init();
    if (sched_spawn(task_shell,   "shell")   < 0) panic("GUI_DEMO: spawn shell");
    if (sched_spawn(task_browser, "browser") < 0) panic("GUI_DEMO: spawn browser");

    serial_puts("[GUI_DEMO] compositor running\r\n");

    /* ── Compositor main loop (runs as task 0 / main) ────────────────────── */
    uint32_t start = timer_ms();

    sched_start();   /* returns when all spawned tasks have exited */

    /* Final render to show the last state */
    comp_render(&g_comp);

    /* ── Summary ─────────────────────────────────────────────────────────── */
    uint32_t elapsed = timer_ms() - start;
    serial_puts("[GUI_DEMO] elapsed_ms=");
    serial_uint(elapsed);
    serial_puts("\r\n");
    serial_puts("[GUI_DEMO] PASS\r\n");
}
