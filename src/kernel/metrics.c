/*
 * ChimaeraOS — Kernel Metrics Subsystem
 * src/kernel/metrics.c
 *
 * See src/include/metrics.h for the full specification.
 *
 * 64-bit division avoidance
 * -------------------------
 * GCC in a freestanding 32-bit build has no libgcc (__udivdi3).
 * metrics_init() computes boot_ms using only 32-bit arithmetic:
 *   delta_lo = (uint32_t)(now_tsc_lo - boot_tsc_lo)
 *   boot_ms  = delta_lo / ticks_per_ms
 *
 * This is safe because the boot sequence takes < 1 second, so the
 * 32-bit low half of the TSC delta never overflows.
 */

#include "../include/metrics.h"
#include "../include/timer.h"
#include "../include/mm.h"
#include "../include/serial.h"
#include "../include/types.h"

/* ── Extern globals set by boot.asm ─────────────────────────────────────── */

extern uint32_t g_boot_tsc_lo;
extern uint32_t g_boot_tsc_hi;

/* ── Port I/O helpers ────────────────────────────────────────────────────── */

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

#define COM1_BASE 0x3F8

/* ── Serial helpers ──────────────────────────────────────────────────────── */

static void m_puts(const char *s)
{
    serial_puts(s);
}

static void m_u32(uint32_t v)
{
    char buf[12];
    int i = 0;
    if (v == 0) { serial_putchar('0'); return; }
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    for (int j = i - 1; j >= 0; j--) serial_putchar(buf[j]);
}

/* ── Metric storage ──────────────────────────────────────────────────────── */

static uint32_t g_boot_ms            = 0;
static uint32_t g_mem_after_boot     = 0;
static uint32_t g_ttfb_ms            = 0;
static uint32_t g_tokens_per_sec     = 0;
static uint32_t g_mem_after_infer    = 0;
static uint32_t g_infer_start_ms     = 0;
static bool     g_infer_started      = false;
static bool     g_first_token_done   = false;

/* ── mm_heap_used forward declaration ────────────────────────────────────── */
extern uint32_t mm_heap_used(void);

/* ── metrics_init ────────────────────────────────────────────────────────── */

void metrics_init(void)
{
    /*
     * Compute boot_ms using 32-bit arithmetic only.
     * g_boot_tsc_lo is the low 32 bits of the TSC at _start (set by boot.asm).
     * timer_tsc() returns the current 64-bit TSC; we use only its low 32 bits.
     * The delta fits in uint32_t because boot takes < 1 second.
     */
    uint32_t now_lo = (uint32_t)timer_tsc();   /* low 32 bits of current TSC */
    uint32_t tpm    = timer_ticks_per_ms();

    uint32_t delta = now_lo - g_boot_tsc_lo;   /* 32-bit subtraction (wraps OK) */

    if (tpm > 0) {
        g_boot_ms = delta / tpm;               /* 32-bit division */
    } else {
        g_boot_ms = timer_ms();
    }
}

/* ── metrics_snapshot_mem_boot ───────────────────────────────────────────── */

void metrics_snapshot_mem_boot(void)
{
    g_mem_after_boot = mm_heap_used();
}

/* ── metrics_start_inference ─────────────────────────────────────────────── */

void metrics_start_inference(void)
{
    g_infer_start_ms   = timer_ms();
    g_infer_started    = true;
    g_first_token_done = false;
}

/* ── metrics_record_first_token ──────────────────────────────────────────── */

void metrics_record_first_token(void)
{
    if (g_infer_started && !g_first_token_done) {
        g_ttfb_ms = timer_ms() - g_infer_start_ms;
        g_first_token_done = true;
    }
}

/* ── metrics_record_token_batch ──────────────────────────────────────────── */

void metrics_record_token_batch(uint32_t n_tokens, uint32_t elapsed_ms)
{
    if (elapsed_ms > 0) {
        g_tokens_per_sec = (n_tokens * 1000u) / elapsed_ms;
    } else {
        g_tokens_per_sec = 999999u;
    }
}

/* ── metrics_snapshot_mem_infer ──────────────────────────────────────────── */

void metrics_snapshot_mem_infer(void)
{
    g_mem_after_infer = mm_heap_used();
}

/* ── metrics_emit ────────────────────────────────────────────────────────── */

void metrics_emit(void)
{
    m_puts("[METRICS] boot_ms=");               m_u32(g_boot_ms);           m_puts("\n");
    m_puts("[METRICS] mem_after_boot_bytes=");  m_u32(g_mem_after_boot);    m_puts("\n");
    m_puts("[METRICS] ttfb_ms=");               m_u32(g_ttfb_ms);           m_puts("\n");
    m_puts("[METRICS] tokens_per_sec=");        m_u32(g_tokens_per_sec);    m_puts("\n");
    m_puts("[METRICS] mem_after_infer_bytes="); m_u32(g_mem_after_infer);   m_puts("\n");
    /* net_rtt_ms is measured host-side; kernel emits 0 as sentinel */
    m_puts("[METRICS] net_rtt_ms=0\n");
    m_puts("[METRICS] DONE\n");
}

/* ── metrics_poll_command ────────────────────────────────────────────────── */

void metrics_poll_command(void)
{
    static char rx_buf[8] = {0};
    static const char CMD[8] = {'m','e','t','r','i','c','s','\n'};

    if (!(inb(COM1_BASE + 5) & 0x01)) return;

    char c = (char)inb(COM1_BASE);

    for (int i = 0; i < 7; i++) rx_buf[i] = rx_buf[i + 1];
    rx_buf[7] = c;

    bool match = true;
    for (int i = 0; i < 8; i++) {
        if (rx_buf[i] != CMD[i]) { match = false; break; }
    }
    if (match) metrics_emit();
}
