/*
 * ChimaeraOS — ACPI Subsystem
 * src/include/acpi.h
 *
 * Provides ACPI table discovery (RSDP → RSDT/XSDT → FADT) and two
 * power-management operations:
 *
 *   acpi_shutdown()  — write SLP_TYPa | SLP_EN to PM1a_CNT_BLK, which
 *                      causes QEMU (and real hardware) to power off.
 *
 *   acpi_reboot()    — try the ACPI reset register first; fall back to
 *                      the keyboard controller (port 0x64 pulse) which
 *                      is universally supported on x86.
 *
 * Initialisation
 * --------------
 *   Call acpi_init() once, early in kernel_main, before any power
 *   management calls.  It scans the BIOS ROM area for the RSDP, then
 *   walks RSDT/XSDT to locate the FADT and extract the required fields.
 *
 *   On success acpi_init() returns true and emits:
 *     [ACPI] RSDP found at 0x<addr>
 *     [ACPI] FADT: PM1a_CNT=0x<port> SLP_TYPa=0x<val> SMI_CMD=0x<port>
 *
 *   On failure (no RSDP, bad checksums, no FADT) it returns false and
 *   acpi_shutdown() / acpi_reboot() fall back to safe alternatives.
 *
 * QEMU notes
 * ----------
 *   QEMU's ACPI tables live in the BIOS area and are fully spec-compliant.
 *   The RSDP is always found in the range 0xE0000–0xFFFFF.
 *   PM1a_CNT_BLK is typically 0x0604; SLP_TYPa for S5 is 0 (bits 13:10).
 *   Writing (SLP_TYPa << 10) | 0x2000 (SLP_EN) to PM1a_CNT_BLK powers off.
 */

#ifndef ACPI_H
#define ACPI_H

#include "types.h"

/* ── Public API ──────────────────────────────────────────────────────────── */

/*
 * acpi_init — scan for RSDP, parse RSDT/XSDT and FADT.
 * Returns true if ACPI is available and shutdown/reboot will use ACPI paths.
 * Returns false if no RSDP found; fallbacks are still available.
 * Must be called before acpi_shutdown() or acpi_reboot().
 */
bool acpi_init(void);

/*
 * acpi_shutdown — power off the machine.
 *
 * Method 1 (ACPI S5): write SLP_TYPa | SLP_EN to PM1a_CNT_BLK.
 *   If a PM1b_CNT_BLK is also present, write to it too.
 * Method 2 (QEMU ISA debug exit): write 0x31 to port 0x501 (exit code 0).
 * Method 3 (last resort): triple-fault via loading a null IDT and INT 0.
 *
 * This function does not return on success.
 */
void acpi_shutdown(void);

/*
 * acpi_reboot — restart the machine.
 *
 * Method 1 (ACPI reset register): write ACPI reset value to reset register
 *   address (FADT revision ≥ 2 only).
 * Method 2 (keyboard controller): pulse the CPU reset line via port 0x64/0x60.
 * Method 3 (triple fault): load null IDT and execute INT 0.
 *
 * This function does not return on success.
 */
void acpi_reboot(void);

/*
 * acpi_available — returns true if acpi_init() succeeded.
 */
bool acpi_available(void);

#endif /* ACPI_H */
