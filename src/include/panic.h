#ifndef PANIC_H
#define PANIC_H

/*
 * ChimaeraOS — Kernel Panic Handler
 * include/panic.h
 *
 * Public API for the kernel panic subsystem.
 *
 * Usage
 * -----
 *   panic("message");              — panic with a static string
 *   panic("msg: val=%d", val);     — NOT supported (no printf); use panic()
 *                                    with a pre-formatted string via serial_*
 *
 * The PANIC() macro captures the current register state via inline assembly
 * and calls panic_impl(), which:
 *   1. Disables interrupts (cli)
 *   2. Dumps all general-purpose registers, EIP, EFLAGS, CR0–CR3
 *   3. Walks the frame-pointer chain (up to 32 frames) and resolves
 *      each return address to a symbol name using the embedded .symtab
 *   4. Flushes the serial FIFO
 *   5. Halts with a recognisable pattern (EAX=0xDEADC0DE, hlt loop)
 *
 * Symbol lookup
 * -------------
 *   symtab_init() must be called once at boot (after mm_init) to copy the
 *   embedded .symtab/.strtab into the kernel heap and sort the entries by
 *   value for O(log n) lookup.  If it is not called, panic still works but
 *   prints raw addresses instead of names.
 */

#include "types.h"

/* ── Register snapshot ──────────────────────────────────────────────────── */

typedef struct {
    uint32_t eax, ebx, ecx, edx;
    uint32_t esi, edi, ebp, esp;
    uint32_t eip;
    uint32_t eflags;
    uint32_t cr0, cr2, cr3;
} panic_regs_t;

/* ── Public API ─────────────────────────────────────────────────────────── */

/*
 * symtab_init — parse the embedded ELF symbol table and prepare it for
 * lookup.  Call once, early in kernel_main, after mm_init().
 */
void symtab_init(void);

/*
 * panic_impl — the real panic implementation.  Do not call directly;
 * use the panic() macro so register state is captured at the call site.
 */
__attribute__((noreturn))
void panic_impl(const char *msg, const panic_regs_t *regs);

/*
 * panic(msg) — capture registers and invoke panic_impl.
 *
 * The macro uses a GNU C compound statement expression to capture all
 * registers before any compiler-generated code can disturb them.
 * It is safe to call from any context (interrupt or thread).
 */
#define panic(msg)                                                          \
    do {                                                                    \
        panic_regs_t __panic_regs;                                          \
        __asm__ volatile (                                                  \
            /* General-purpose registers */                                 \
            "movl %%eax, %0\n\t"                                            \
            "movl %%ebx, %1\n\t"                                            \
            "movl %%ecx, %2\n\t"                                            \
            "movl %%edx, %3\n\t"                                            \
            "movl %%esi, %4\n\t"                                            \
            "movl %%edi, %5\n\t"                                            \
            "movl %%ebp, %6\n\t"                                            \
            "movl %%esp, %7\n\t"                                            \
            /* EIP: use a label trick — lea the next instruction's addr */  \
            "call 1f\n\t"                                                   \
            "1: popl %8\n\t"                                                \
            /* EFLAGS */                                                     \
            "pushfl\n\t"                                                    \
            "popl  %9\n\t"                                                  \
            : "=m"(__panic_regs.eax),                                       \
              "=m"(__panic_regs.ebx),                                       \
              "=m"(__panic_regs.ecx),                                       \
              "=m"(__panic_regs.edx),                                       \
              "=m"(__panic_regs.esi),                                       \
              "=m"(__panic_regs.edi),                                       \
              "=m"(__panic_regs.ebp),                                       \
              "=m"(__panic_regs.esp),                                       \
              "=m"(__panic_regs.eip),                                       \
              "=m"(__panic_regs.eflags)                                     \
            : /* no inputs */                                               \
            : "memory"                                                      \
        );                                                                  \
        /* Control registers (require ring 0) */                            \
        __asm__ volatile (                                                  \
            "movl %%cr0, %%eax\n\t"                                         \
            "movl %%eax, %0\n\t"                                            \
            "movl %%cr2, %%eax\n\t"                                         \
            "movl %%eax, %1\n\t"                                            \
            "movl %%cr3, %%eax\n\t"                                         \
            "movl %%eax, %2\n\t"                                            \
            : "=m"(__panic_regs.cr0),                                       \
              "=m"(__panic_regs.cr2),                                       \
              "=m"(__panic_regs.cr3)                                        \
            : : "eax", "memory"                                             \
        );                                                                  \
        panic_impl((msg), &__panic_regs);                                   \
    } while (0)

#endif /* PANIC_H */
