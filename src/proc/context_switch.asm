; ChimaeraOS — Task Entry Trampoline
; src/proc/context_switch.asm
;
; Exports:
;   task_entry_trampoline  — naked entry point for new tasks
;   context_switch         — kept as a no-op stub (not used in the int-based design)
;
; The scheduler now uses "int 0x30" for cooperative yield and IRQ0 for
; preemptive yield.  Both paths use the same interrupt frame format and
; are handled entirely in idt.asm.  context_switch() is no longer called.
;
; task_entry_trampoline
; ---------------------
; When a new task is first resumed via IRET from yield_int_handler or
; irq0_handler, execution jumps here.  At this point:
;
;   EBX = entry function pointer (set by sched_spawn in the initial frame)
;   Interrupts are ENABLED (EFLAGS.IF=1 was in the IRET frame)
;   ESP = task's kernel stack (after IRET popped EIP/CS/EFLAGS)
;
; We call task_trampoline_c(entry) — a C function that calls entry() and
; handles task exit.  We pass EBX as the first cdecl argument.

bits 32
section .text

global context_switch
global task_entry_trampoline
extern task_trampoline_c    ; C function: void task_trampoline_c(void (*entry)(void))

; ── context_switch (stub — not used in int-based design) ─────────────────────
;
; void context_switch(uint32_t *save_esp, uint32_t load_esp);
; Kept for link compatibility.  Should not be called.

context_switch:
    ret

; ── task_entry_trampoline ─────────────────────────────────────────────────────
;
; Entry point for newly spawned tasks.
; Jumped to via IRET when the task is first scheduled.
;
; EBX = entry function pointer (from the initial stack frame built by sched_spawn)
; Interrupts are already enabled (IF=1 in the IRET'd EFLAGS)
;
; We call task_trampoline_c(EBX) using cdecl:
;   push EBX (arg0)
;   push 0   (fake return address — task_trampoline_c never returns)
;   jmp  task_trampoline_c

task_entry_trampoline:
    push ebx                ; arg0 = entry function pointer
    push dword 0            ; fake return address
    jmp  task_trampoline_c  ; tail-call (never returns)
