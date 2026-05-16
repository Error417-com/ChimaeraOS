/*
 * ChimaeraOS — Kernel Metrics Subsystem
 * src/include/metrics.h
 *
 * Tracks five performance metrics and emits them over the serial port in a
 * structured format that scripts/perf_collect.py can parse into CSV.
 *
 * Metric definitions
 * ------------------
 *
 *   boot_ms
 *     Milliseconds from multiboot entry (_start) to the first
 *     [METRICS] READY line.  Measured by recording the TSC at _start
 *     (stored in a well-known global by boot.asm) and comparing it to
 *     the TSC at the point metrics_record_boot() is called.
 *
 *   ttfb_ms
 *     Time-to-first-byte: milliseconds from LLM prompt submission to
 *     receipt of the first output token.  Measured by
 *     metrics_start_inference() / metrics_record_first_token().
 *     In the integration kernel this is simulated with a synthetic
 *     busy-loop; in the LLM kernel it wraps the real inference path.
 *
 *   tokens_per_sec
 *     Throughput over a 100-token generation window.  Measured by
 *     metrics_record_token_batch(n_tokens, elapsed_ms).
 *
 *   mem_after_boot_bytes
 *     Heap bytes consumed after boot initialisation completes
 *     (after serial_init, mm_init, symtab_init, timer_init, ata_init,
 *     fat32_mount).  Captured by metrics_snapshot_mem_boot().
 *
 *   mem_after_infer_bytes
 *     Heap bytes consumed after the first inference run completes.
 *     Captured by metrics_snapshot_mem_infer().
 *
 * Serial output format
 * --------------------
 * All metric lines begin with the prefix "[METRICS] " followed by a
 * key=value pair.  The final line is "[METRICS] DONE".
 *
 *   [METRICS] boot_ms=<uint32>
 *   [METRICS] mem_after_boot_bytes=<uint32>
 *   [METRICS] ttfb_ms=<uint32>
 *   [METRICS] tokens_per_sec=<uint32>
 *   [METRICS] mem_after_infer_bytes=<uint32>
 *   [METRICS] DONE
 *
 * /metrics serial command
 * -----------------------
 * The kernel also listens for the ASCII string "metrics\n" on the serial
 * RX line.  When received, it re-emits the same block.  This allows the
 * host to query metrics at any time after boot without rebooting.
 * See metrics_poll_command() which must be called from the kernel main loop.
 */
#ifndef METRICS_H
#define METRICS_H

#include "types.h"

/*
 * metrics_init()
 * Initialise the metrics subsystem.  Must be called after timer_init().
 * Records the TSC at call time as the "boot entry" reference point.
 * (boot.asm stores the raw TSC at _start in g_boot_tsc_lo/hi; this
 * function reads those globals to compute boot_ms accurately.)
 */
void metrics_init(void);

/*
 * metrics_snapshot_mem_boot()
 * Capture the heap-used value immediately after boot initialisation.
 * Call after fat32_mount() returns.
 */
void metrics_snapshot_mem_boot(void);

/*
 * metrics_start_inference()
 * Mark the start of an LLM inference request (for ttfb_ms measurement).
 */
void metrics_start_inference(void);

/*
 * metrics_record_first_token()
 * Mark receipt of the first output token (completes ttfb_ms measurement).
 */
void metrics_record_first_token(void);

/*
 * metrics_record_token_batch(n_tokens, elapsed_ms)
 * Record throughput: n_tokens were generated in elapsed_ms milliseconds.
 * tokens_per_sec = (n_tokens * 1000) / elapsed_ms.
 */
void metrics_record_token_batch(uint32_t n_tokens, uint32_t elapsed_ms);

/*
 * metrics_snapshot_mem_infer()
 * Capture the heap-used value after the first inference completes.
 */
void metrics_snapshot_mem_infer(void);

/*
 * metrics_emit()
 * Print all collected metrics to the serial port in the structured format
 * described above.  Safe to call multiple times.
 */
void metrics_emit(void);

/*
 * metrics_poll_command()
 * Non-blocking check for "metrics\n" on the serial RX line.
 * If the command is detected, calls metrics_emit().
 * Call from the kernel main loop or idle path.
 */
void metrics_poll_command(void);

#endif /* METRICS_H */
