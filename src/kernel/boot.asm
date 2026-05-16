; ChimaeraOS - Multiboot Entry Point
; kernel/boot.asm
;
; Provides the multiboot header so GRUB can load the kernel, then
; sets up a minimal stack and calls kernel_main().
;
; Metrics: records the TSC at _start in g_boot_tsc_lo / g_boot_tsc_hi
; (two 32-bit globals declared in metrics.c / extern'd from boot.asm).
; This allows metrics_init() to compute boot_ms accurately.

MBOOT_MAGIC    equ 0x1BADB002
MBOOT_FLAGS    equ 0x00000003   ; align modules + memory map
MBOOT_CHECKSUM equ -(MBOOT_MAGIC + MBOOT_FLAGS)

section .multiboot
align 4
    dd MBOOT_MAGIC
    dd MBOOT_FLAGS
    dd MBOOT_CHECKSUM

section .bss
align 16
stack_bottom:
    resb 65536          ; 64 KiB kernel stack
stack_top:

section .data
global g_boot_tsc_lo
global g_boot_tsc_hi
g_boot_tsc_lo: dd 0
g_boot_tsc_hi: dd 0

section .text
global _start
extern kernel_main

_start:
    ; Record TSC as early as possible (before stack setup)
    rdtsc                       ; EDX:EAX = TSC
    mov [g_boot_tsc_lo], eax
    mov [g_boot_tsc_hi], edx

    ; Set up stack
    mov esp, stack_top

    ; Clear EFLAGS
    push 0
    popf

    ; Call kernel_main (no arguments)
    call kernel_main

    ; Should never return — halt
.halt:
    cli
    hlt
    jmp .halt
