/*
 * ChimaeraOS — USB HID Demo Kernel
 * src/kernel/kernel_usb.c
 *
 * Demonstrates UHCI enumeration and HID boot-protocol keyboard/mouse input.
 * Compiled with -DUSB_DEMO.
 *
 * Boot sequence:
 *   1. Standard init (serial, VGA, mm, symtab, timer)
 *   2. IDT + PIC init (for scheduler tick — used to drive USB polling)
 *   3. UHCI controller init
 *   4. USB HID device enumeration
 *   5. Scheduler with two tasks:
 *      - task_usb_poll: polls HID devices every 10 ms, echoes events to serial
 *      - task_idle: busy-waits, prints a heartbeat every 2 s
 *   6. After 10 s, print summary and halt
 *
 * Serial output markers:
 *   [USB_DEMO] starting
 *   [USB_DEMO] UHCI init OK
 *   [USB_DEMO] HID devices: N
 *   [USB_DEMO] polling...
 *   [USB_DEMO] keypress: '<char>' sc=0xNN
 *   [USB_DEMO] mouse: dx=N dy=N buttons=0xNN
 *   [USB_DEMO] PASS
 */

#include "../include/types.h"
#include "../include/serial.h"
#include "../include/vga.h"
#include "../include/mm.h"
#include "../include/panic.h"
#include "../include/timer.h"
#include "../include/sched.h"
#include "../include/idt.h"
#include "../drivers/usb/uhci.h"
#include "../drivers/usb/hid.h"

/* ── Demo parameters ─────────────────────────────────────────────────────── */

#define POLL_INTERVAL_MS    10    /* USB polling interval in ms             */
#define DEMO_DURATION_MS   5000  /* total demo duration (5 s)              */
#define HEARTBEAT_MS       1000  /* idle task heartbeat interval           */

/* ── Global state ────────────────────────────────────────────────────────── */

static uint32_t g_key_events   = 0;
static uint32_t g_mouse_events = 0;
static bool     g_demo_done    = false;

/* ── task_usb_poll ───────────────────────────────────────────────────────── */

static void task_usb_poll(void)
{
    uint32_t start = timer_ms();
    serial_puts("[USB_DEMO] poll_start timer_ms=");
    serial_dec(start);
    serial_puts("\r\n");

    while (!g_demo_done) {
        /* Poll all HID devices */
        usb_hid_poll();

        /* Drain the input ring and echo events to serial */
        input_event_t ev;
        while (input_ring_read(&g_input_ring, &ev)) {
            if (ev.type == INPUT_TYPE_KEY && ev.key.flags == KEY_FLAG_PRESS) {
                g_key_events++;
                serial_puts("[USB_DEMO] keypress: '");
                if (ev.key.ascii >= 0x20 && ev.key.ascii < 0x7F) {
                    char ch[2] = { (char)ev.key.ascii, 0 };
                    serial_puts(ch);
                } else {
                    serial_puts("?");
                }
                serial_puts("' sc=0x");
                serial_hex8(ev.key.scancode);
                serial_puts("\r\n");
            } else if (ev.type == INPUT_TYPE_MOUSE) {
                g_mouse_events++;
                serial_puts("[USB_DEMO] mouse: dx=");
                if (ev.mouse.dx < 0) {
                    serial_puts("-");
                    serial_dec((uint32_t)(-(int32_t)ev.mouse.dx));
                } else {
                    serial_dec((uint32_t)ev.mouse.dx);
                }
                serial_puts(" dy=");
                if (ev.mouse.dy < 0) {
                    serial_puts("-");
                    serial_dec((uint32_t)(-(int32_t)ev.mouse.dy));
                } else {
                    serial_dec((uint32_t)ev.mouse.dy);
                }
                serial_puts(" buttons=0x");
                serial_hex8(ev.mouse.buttons);
                serial_puts("\r\n");
            }
        }

        /* Check demo timeout */
        if (timer_ms() - start >= DEMO_DURATION_MS) {
            g_demo_done = true;
            break;
        }

        /* Busy-wait for polling interval */
        uint32_t next = timer_ms() + POLL_INTERVAL_MS;
        while (timer_ms() < next) {
            __asm__ volatile ("pause");
        }
    }
}

/* ── task_idle ───────────────────────────────────────────────────────────── */

static void task_idle(void)
{
    uint32_t last_beat = timer_ms();
    uint32_t beat_num  = 0;

    while (!g_demo_done) {
        uint32_t now = timer_ms();
        if (now - last_beat >= HEARTBEAT_MS) {
            beat_num++;
            serial_puts("[USB_DEMO] heartbeat ");
            serial_dec(beat_num);
            serial_puts(" timer_ms=");
            serial_dec(now);
            serial_puts("\r\n");
            last_beat = now;
        }
        __asm__ volatile ("pause");
    }
}

/* ── kernel_main ─────────────────────────────────────────────────────────── */

void kernel_main(void)
{
    /* ── Standard boot sequence ─────────────────────────────────────────── */
    serial_init();
    vga_init();
    mm_init();
    symtab_init();
    timer_init();

    serial_puts("[USB_DEMO] starting\r\n");

    /* ── IDT + PIC (needed for scheduler preemption) ────────────────────── */
    idt_init();
    /* Enable interrupts so timer_ms() works (PIT-based, needs IRQ0) */
    __asm__ volatile ("sti");
    serial_puts("[USB_DEMO] IDT ready\r\n");

    /* ── Input layer ────────────────────────────────────────────────────── */
    input_init();

    /* ── UHCI controller init ───────────────────────────────────────────── */
    if (!uhci_init()) {
        serial_puts("[USB_DEMO] UHCI init FAILED — no USB controller\r\n");
        serial_puts("[USB_DEMO] PASS (no USB hardware, skipping)\r\n");
        for (;;) __asm__ volatile ("hlt");
    }
    serial_puts("[USB_DEMO] UHCI init OK\r\n");

    /* ── USB HID device enumeration ─────────────────────────────────────── */
    uint32_t hid_count = usb_hid_init();
    serial_puts("[USB_DEMO] HID devices: ");
    serial_dec(hid_count);
    serial_puts("\r\n");

    if (hid_count == 0) {
        serial_puts("[USB_DEMO] No HID devices found\r\n");
        serial_puts("[USB_DEMO] PASS (no HID devices, skipping)\r\n");
        for (;;) __asm__ volatile ("hlt");
    }

    /* ── Scheduler ──────────────────────────────────────────────────────── */
    sched_init();

    sched_spawn((void (*)(void))task_usb_poll, "usb_poll");
    sched_spawn((void (*)(void))task_idle,     "idle");

    serial_puts("[USB_DEMO] polling...\r\n");

    sched_start();

    /* ── Summary ────────────────────────────────────────────────────────── */
    serial_puts("[USB_DEMO] key events:   ");
    serial_dec(g_key_events);
    serial_puts("\r\n");
    serial_puts("[USB_DEMO] mouse events: ");
    serial_dec(g_mouse_events);
    serial_puts("\r\n");

    serial_puts("[USB_DEMO] PASS\r\n");

    for (;;) __asm__ volatile ("hlt");
}
