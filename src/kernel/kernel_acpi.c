/*
 * ChimaeraOS — ACPI Demo Kernel
 * src/kernel/kernel_acpi.c
 *
 * Compiled with -DACPI_DEMO.
 *
 * Boots, initialises ACPI, then runs a minimal serial shell that accepts
 * three commands on COM1 (115200 8N1):
 *
 *   shutdown\n  — calls acpi_shutdown()  (powers off QEMU)
 *   reboot\n    — calls acpi_reboot()    (restarts QEMU)
 *   status\n    — prints ACPI status to serial
 *
 * The test harness (acpi_test.py) sends these commands via the QEMU
 * monitor's "chardev-send" facility and checks the serial log for the
 * expected output markers.
 *
 * Serial output markers
 * ---------------------
 *   [ACPI_DEMO] starting
 *   [ACPI_DEMO] ACPI ok / ACPI unavailable
 *   [ACPI_DEMO] shell ready
 *   [ACPI_DEMO] cmd: shutdown
 *   [ACPI_DEMO] cmd: reboot
 *   [ACPI_DEMO] cmd: status
 *   [ACPI] Shutdown requested          (from acpi.c)
 *   [ACPI] Reboot requested            (from acpi.c)
 *
 * The test harness verifies:
 *   1. RSDP found line appears
 *   2. FADT parsed line appears
 *   3. Shell ready line appears
 *   4. After "shutdown" command: "[ACPI] Shutdown requested" appears
 *      and QEMU exits (process terminates)
 *   5. After "reboot" command: "[ACPI] Reboot requested" appears
 *      and QEMU restarts (detected by re-appearance of boot banner)
 */

#include "../include/serial.h"
#include "../include/vga.h"
#include "../include/mm.h"
#include "../include/panic.h"
#include "../include/timer.h"
#include "../include/acpi.h"
#include "../include/types.h"

/* ── Port I/O (COM1 RX) ──────────────────────────────────────────────────── */

#define COM1_BASE  0x3F8

static inline uint8_t _inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* Returns true if a byte is available on COM1 RX */
static bool serial_rx_ready(void)
{
    return (_inb(COM1_BASE + 5) & 0x01) != 0;
}

/* Read one byte from COM1 (blocking) */
static char serial_getchar(void)
{
    while (!serial_rx_ready()) {
        __asm__ volatile ("pause");
    }
    return (char)_inb(COM1_BASE);
}

/* ── Shell command buffer ────────────────────────────────────────────────── */

#define CMD_BUF_SIZE  16

static char  g_cmd_buf[CMD_BUF_SIZE];
static uint8_t g_cmd_len = 0;

/* Reset the command buffer */
static void cmd_reset(void)
{
    g_cmd_len = 0;
    for (int i = 0; i < CMD_BUF_SIZE; i++) g_cmd_buf[i] = '\0';
}

/* Compare g_cmd_buf against a literal string (null-terminated) */
static bool cmd_is(const char *s)
{
    uint8_t i = 0;
    while (s[i] && i < CMD_BUF_SIZE) {
        if (g_cmd_buf[i] != s[i]) return false;
        i++;
    }
    return (s[i] == '\0' && i == g_cmd_len);
}

/* ── Shell dispatch ───────────────────────────────────────────────────────── */

static void shell_dispatch(void)
{
    if (cmd_is("shutdown")) {
        serial_puts("[ACPI_DEMO] cmd: shutdown\n");
        acpi_shutdown();
        /* acpi_shutdown() does not return */
    } else if (cmd_is("reboot")) {
        serial_puts("[ACPI_DEMO] cmd: reboot\n");
        acpi_reboot();
        /* acpi_reboot() does not return */
    } else if (cmd_is("status")) {
        serial_puts("[ACPI_DEMO] cmd: status\n");
        if (acpi_available()) {
            serial_puts("[ACPI_DEMO] status: ACPI available\n");
        } else {
            serial_puts("[ACPI_DEMO] status: ACPI unavailable (fallbacks active)\n");
        }
    } else if (g_cmd_len > 0) {
        serial_puts("[ACPI_DEMO] unknown command: '");
        for (uint8_t i = 0; i < g_cmd_len; i++) serial_putchar(g_cmd_buf[i]);
        serial_puts("'\n");
    }
}

/* ── Shell main loop ─────────────────────────────────────────────────────── */

static void shell_run(void)
{
    serial_puts("[ACPI_DEMO] shell ready\n");
    serial_puts("[ACPI_DEMO] commands: shutdown, reboot, status\n");

    cmd_reset();

    for (;;) {
        char c = serial_getchar();

        if (c == '\n' || c == '\r') {
            /* End of command — dispatch */
            shell_dispatch();
            cmd_reset();
        } else if (c == '\b' || c == 0x7F) {
            /* Backspace */
            if (g_cmd_len > 0) g_cmd_len--;
        } else if (g_cmd_len < CMD_BUF_SIZE - 1) {
            g_cmd_buf[g_cmd_len++] = c;
        }
        /* Bytes beyond CMD_BUF_SIZE are silently dropped */
    }
}

/* ── kernel_main ─────────────────────────────────────────────────────────── */

void kernel_main(void)
{
    serial_init();
    vga_init();
    mm_init();
    symtab_init();
    timer_init();

    serial_puts("[ACPI_DEMO] starting\n");
    vga_puts("ChimeraOS ACPI Demo\n");
    vga_puts("Send 'shutdown' or 'reboot' on COM1\n");

    /* Initialise ACPI — scan RSDP, parse RSDT/FADT */
    bool acpi_ok = acpi_init();
    if (acpi_ok) {
        serial_puts("[ACPI_DEMO] ACPI ok\n");
        vga_puts("ACPI: OK\n");
    } else {
        serial_puts("[ACPI_DEMO] ACPI unavailable (fallbacks will be used)\n");
        vga_puts("ACPI: unavailable\n");
    }

    /* Run the interactive shell */
    shell_run();

    /* shell_run() only returns if something goes very wrong */
    for (;;) __asm__ volatile ("hlt");
}
