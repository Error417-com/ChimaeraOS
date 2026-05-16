/*
 * ChimaeraOS — Kernel Panic Handler
 * src/kernel/panic.c
 *
 * Implements:
 *   symtab_init()   — parse embedded ELF .symtab/.strtab at boot
 *   panic_impl()    — called via the panic() macro; dumps registers,
 *                     walks the stack with symbol names, then halts
 *
 * Design notes
 * ------------
 * The .symtab and .strtab sections are embedded in the kernel image as
 * LOAD segments (see linker.ld).  The linker exports four symbols:
 *
 *   __symtab_start / __symtab_end   — bounds of the raw Elf32_Sym array
 *   __strtab_start / __strtab_end   — bounds of the string table
 *
 * symtab_init() copies the function-typed entries into a heap-allocated
 * array sorted by value so that lookup_symbol() can do a linear scan for
 * the nearest symbol <= the query address.  (The table is small enough
 * that a linear scan is fine; a binary search would be trivial to add.)
 *
 * Stack walking
 * -------------
 * The kernel is compiled with -fno-omit-frame-pointer (added to CFLAGS).
 * Each stack frame therefore has the layout:
 *
 *   [ebp+0]  saved EBP of caller
 *   [ebp+4]  return address into caller
 *
 * panic_impl() receives the EBP captured at the panic() call site and
 * walks up to PANIC_MAX_FRAMES frames.
 *
 * Halt pattern
 * ------------
 * After printing the dump the handler:
 *   1. Writes the magic value 0xDEADC0DE to EAX (visible in QEMU monitor).
 *   2. Writes 0x31 to I/O port 0x501 (QEMU isa-debug-exit device, if
 *      present) — this causes QEMU to exit with status (0x31<<1)|1 = 99,
 *      which is distinct from a normal exit and easy to detect in CI.
 *   3. Spins on `hlt` with interrupts disabled so the CPU never executes
 *      another instruction.
 *
 * Serial flush
 * ------------
 * serial_flush() polls the UART LSR until the transmitter holding register
 * and shift register are both empty (LSR bits 5 and 6 both set).
 */

#include "../include/panic.h"
#include "../include/serial.h"
#include "../include/mm.h"
#include "../include/types.h"

/* ── ELF32 type definitions (self-contained, no system headers) ─────────── */

typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Off;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Word;
typedef int32_t  Elf32_Sword;

/* ELF symbol table entry */
typedef struct {
    Elf32_Word  st_name;    /* index into string table */
    Elf32_Addr  st_value;   /* symbol value (address) */
    Elf32_Word  st_size;    /* symbol size in bytes */
    uint8_t     st_info;    /* type and binding */
    uint8_t     st_other;   /* visibility */
    Elf32_Half  st_shndx;   /* section index */
} Elf32_Sym;

#define ELF32_ST_TYPE(i)  ((i) & 0xf)
#define STT_FUNC          2
#define STN_UNDEF         0

/* ── Linker-exported symbol table bounds ────────────────────────────────── */

extern char __symtab_start[];
extern char __symtab_end[];
extern char __strtab_start[];
extern char __strtab_end[];

/* ── Internal symbol table (sorted copy in heap) ────────────────────────── */

#define PANIC_MAX_SYMS    512   /* max function symbols to keep */
#define PANIC_MAX_FRAMES  32    /* max stack frames to walk */

typedef struct {
    uint32_t    addr;
    uint32_t    size;
    const char *name;   /* pointer into heap-copied strtab */
} sym_entry_t;

static sym_entry_t *g_syms      = NULL;
static uint32_t     g_sym_count = 0;
static char        *g_strtab    = NULL;
static uint32_t     g_strtab_sz = 0;

/* ── Serial helpers (avoid pulling in printf) ───────────────────────────── */

#define COM1_BASE 0x3F8

static inline uint8_t _inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void _outb(uint16_t port, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

/* Block until the UART transmitter is completely empty. */
static void serial_flush(void)
{
    /* Wait for THRE (bit 5) and TEMT (bit 6) */
    while ((_inb(COM1_BASE + 5) & 0x60) != 0x60) {}
}

/* ── symtab_init ─────────────────────────────────────────────────────────── */

/*
 * Simple insertion sort — the symbol table is small (< 200 entries) so
 * O(n²) is fine and avoids needing qsort.
 */
static void sort_syms(sym_entry_t *arr, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++) {
        sym_entry_t key = arr[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && arr[j].addr > key.addr) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

void symtab_init(void)
{
    /* Locate the raw symbol table embedded by the linker. */
    const Elf32_Sym *raw_syms = (const Elf32_Sym *)(uintptr_t)__symtab_start;
    uint32_t raw_sz  = (uint32_t)((uintptr_t)__symtab_end - (uintptr_t)__symtab_start);
    uint32_t n_raw   = raw_sz / sizeof(Elf32_Sym);

    uint32_t strtab_sz = (uint32_t)((uintptr_t)__strtab_end - (uintptr_t)__strtab_start);

    if (n_raw == 0 || strtab_sz == 0) {
        serial_puts("[SYMTAB] symtab_init: no symbol table embedded\n");
        return;
    }

    /* Copy the string table into the heap so it survives. */
    g_strtab = (char *)kmalloc(strtab_sz + 1);
    if (!g_strtab) {
        serial_puts("[SYMTAB] symtab_init: kmalloc strtab failed\n");
        return;
    }
    for (uint32_t i = 0; i < strtab_sz; i++)
        g_strtab[i] = __strtab_start[i];
    g_strtab[strtab_sz] = '\0';
    g_strtab_sz = strtab_sz;

    /* Count function symbols (STT_FUNC, defined, non-zero address). */
    uint32_t func_count = 0;
    for (uint32_t i = 0; i < n_raw && func_count < PANIC_MAX_SYMS; i++) {
        const Elf32_Sym *s = &raw_syms[i];
        if (ELF32_ST_TYPE(s->st_info) == STT_FUNC &&
            s->st_shndx != STN_UNDEF &&
            s->st_value != 0)
            func_count++;
    }

    if (func_count == 0) {
        serial_puts("[SYMTAB] symtab_init: no function symbols found\n");
        return;
    }

    g_syms = (sym_entry_t *)kmalloc(func_count * sizeof(sym_entry_t));
    if (!g_syms) {
        serial_puts("[SYMTAB] symtab_init: kmalloc syms failed\n");
        return;
    }

    uint32_t idx = 0;
    for (uint32_t i = 0; i < n_raw && idx < func_count; i++) {
        const Elf32_Sym *s = &raw_syms[i];
        if (ELF32_ST_TYPE(s->st_info) == STT_FUNC &&
            s->st_shndx != STN_UNDEF &&
            s->st_value != 0) {
            g_syms[idx].addr = s->st_value;
            g_syms[idx].size = s->st_size;
            /* Point into our heap copy of the string table. */
            if (s->st_name < g_strtab_sz)
                g_syms[idx].name = &g_strtab[s->st_name];
            else
                g_syms[idx].name = "?";
            idx++;
        }
    }
    g_sym_count = idx;

    sort_syms(g_syms, g_sym_count);

    serial_puts("[SYMTAB] symtab_init: loaded ");
    serial_dec(g_sym_count);
    serial_puts(" function symbols\n");
}

/* ── Symbol lookup ──────────────────────────────────────────────────────── */

/*
 * Return the name of the function that contains `addr`, or NULL if not found.
 * Also sets *offset_out to the byte offset within the function.
 */
static const char *lookup_symbol(uint32_t addr, uint32_t *offset_out)
{
    if (!g_syms || g_sym_count == 0) return NULL;

    /* Linear scan for the last symbol whose value <= addr. */
    const sym_entry_t *best = NULL;
    for (uint32_t i = 0; i < g_sym_count; i++) {
        if (g_syms[i].addr <= addr) {
            best = &g_syms[i];
        } else {
            break;  /* sorted, so we can stop early */
        }
    }

    if (!best) return NULL;

    /* Sanity check: addr must be within the function's reported size,
     * or within a reasonable window (64 KiB) if size is 0. */
    uint32_t limit = best->size ? best->addr + best->size
                                : best->addr + 0x10000u;
    if (addr > limit) return NULL;

    if (offset_out) *offset_out = addr - best->addr;
    return best->name;
}

/* ── Panic implementation ────────────────────────────────────────────────── */

__attribute__((noreturn))
void panic_impl(const char *msg, const panic_regs_t *r)
{
    /* 1. Disable interrupts immediately. */
    __asm__ volatile ("cli");

    /* 2. Banner. */
    serial_puts("\r\n");
    serial_puts("╔══════════════════════════════════════════════════════════╗\r\n");
    serial_puts("║              *** KERNEL PANIC ***                        ║\r\n");
    serial_puts("╚══════════════════════════════════════════════════════════╝\r\n");
    serial_puts("[PANIC] ");
    serial_puts(msg ? msg : "(no message)");
    serial_puts("\r\n\r\n");

    /* 3. Register dump. */
    serial_puts("[PANIC] Registers:\r\n");

    serial_puts("  EAX="); serial_hex32(r->eax);
    serial_puts("  EBX="); serial_hex32(r->ebx);
    serial_puts("  ECX="); serial_hex32(r->ecx);
    serial_puts("  EDX="); serial_hex32(r->edx);
    serial_puts("\r\n");

    serial_puts("  ESI="); serial_hex32(r->esi);
    serial_puts("  EDI="); serial_hex32(r->edi);
    serial_puts("  EBP="); serial_hex32(r->ebp);
    serial_puts("  ESP="); serial_hex32(r->esp);
    serial_puts("\r\n");

    serial_puts("  EIP="); serial_hex32(r->eip);
    serial_puts("  EFLAGS="); serial_hex32(r->eflags);
    serial_puts("\r\n");

    serial_puts("  CR0="); serial_hex32(r->cr0);
    serial_puts("  CR2="); serial_hex32(r->cr2);
    serial_puts("  CR3="); serial_hex32(r->cr3);
    serial_puts("\r\n\r\n");

    /* 4. Stack dump — top 32 dwords from ESP. */
    serial_puts("[PANIC] Stack (top 32 dwords from ESP):\r\n");
    {
        const uint32_t *sp = (const uint32_t *)(uintptr_t)r->esp;
        for (int i = 0; i < 32; i++) {
            /* Guard against reading unmapped memory: stay below 64 MiB. */
            if ((uintptr_t)(sp + i) >= 0x04000000u) break;
            serial_puts("  [ESP+");
            serial_dec((uint32_t)(i * 4));
            serial_puts("] ");
            serial_hex32(sp[i]);

            /* Annotate with symbol name if it looks like a code address. */
            uint32_t offset = 0;
            const char *sym = lookup_symbol(sp[i], &offset);
            if (sym) {
                serial_puts("  <");
                serial_puts(sym);
                serial_puts("+");
                serial_dec(offset);
                serial_puts(">");
            }
            serial_puts("\r\n");
        }
    }
    serial_puts("\r\n");

    /* 5. Stack trace — walk frame pointers. */
    serial_puts("[PANIC] Stack trace:\r\n");
    {
        uint32_t ebp = r->ebp;
        for (int frame = 0; frame < PANIC_MAX_FRAMES; frame++) {
            /* Validate EBP: must be 4-byte aligned and below 64 MiB. */
            if (ebp == 0 || (ebp & 3) || ebp >= 0x04000000u) break;

            uint32_t ret_addr = *(const uint32_t *)(uintptr_t)(ebp + 4);
            uint32_t next_ebp = *(const uint32_t *)(uintptr_t)ebp;

            serial_puts("  #");
            serial_dec((uint32_t)frame);
            serial_puts("  ");
            serial_hex32(ret_addr);

            uint32_t offset = 0;
            const char *sym = lookup_symbol(ret_addr, &offset);
            if (sym) {
                serial_puts("  <");
                serial_puts(sym);
                serial_puts("+");
                serial_dec(offset);
                serial_puts(">");
            } else {
                serial_puts("  <unknown>");
            }
            serial_puts("\r\n");

            /* Detect stack corruption: EBP must increase. */
            if (next_ebp <= ebp) break;
            ebp = next_ebp;
        }
    }
    serial_puts("\r\n");

    /* 6. Flush serial FIFO. */
    serial_puts("[PANIC] System halted.\r\n");
    serial_flush();

    /* 7. Halt with a recognisable pattern.
     *    - Write 0x31 to port 0x501 (QEMU isa-debug-exit, if present).
     *      QEMU exits with status (val<<1)|1 = 99.
     *    - Load EAX=0xDEADC0DE so it's visible in `info registers`.
     *    - Spin on `hlt` with interrupts disabled. */
    __asm__ volatile (
        "cli\n\t"
        /* QEMU isa-debug-exit: port 0x501, value 0x31 → exit status 99.
         * Port 0x501 > 0xFF so we must use the DX form of outb. */
        "movw $0x501, %%dx\n\t"
        "movb $0x31,  %%al\n\t"
        "outb %%al, %%dx\n\t"
        /* Recognisable halt pattern: EAX=0xDEADC0DE visible in QEMU monitor */
        "movl $0xDEADC0DE, %%eax\n\t"
        "1: hlt\n\t"
        "jmp 1b\n\t"
        : : : "eax", "edx"
    );
    __builtin_unreachable();
}
