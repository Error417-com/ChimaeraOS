/* ChimaeraOS - Kernel Linker Script
 *
 * Two-pass build: the Makefile first links a pass-1 ELF (without symbol
 * tables), extracts .symtab/.strtab as binary blobs, converts them to
 * relocatable .o files with sections .symtab_blob and .strtab_blob, then
 * links the final kernel with those blobs included.
 *
 * The panic handler (panic.c) uses the linker-exported symbols
 * __symtab_start/__symtab_end and __strtab_start/__strtab_end to locate
 * the embedded symbol table at runtime.
 */
OUTPUT_FORMAT("elf32-i386")
ENTRY(_start)

SECTIONS
{
    . = 1M;   /* Load at 1 MiB (standard multiboot location) */

    .text ALIGN(4K) :
    {
        *(.multiboot)
        *(.text)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata*)
        *(.eh_frame)
    }

    .data ALIGN(4K) :
    {
        *(.data)
    }

    /* ── Embedded symbol table (LOAD segment, always in memory) ─────────── */
    /* These sections are populated by the two-pass build (embed_symtab.sh). */
    .symtab_load ALIGN(4K) :
    {
        __symtab_start = .;
        *(.symtab_blob)
        __symtab_end   = .;
    }

    .strtab_load ALIGN(4K) :
    {
        __strtab_start = .;
        *(.strtab_blob)
        __strtab_end   = .;
    }

    /* ── BSS (zero-initialised, not stored in file) ─────────────────────── */
    .bss ALIGN(4K) :
    {
        *(COMMON)
        *(.bss)
    }

    /* Expose kernel image bounds */
    __kernel_start = 1M;
    __kernel_end   = .;
}
