; ChimaeraOS — IDT, PIC, and IRQ0 Handler
; src/kernel/idt.asm
;
; Provides:
;   idt_load(idtr_ptr)   — executes LIDT with the given IDTR pointer
;   irq0_handler         — PIT timer interrupt handler (vector 0x20)
;   irq_spurious_handler — default handler for all other vectors
;
; The C code in idt.c builds the IDT entries and calls idt_load().
;
; ── Context switch design ─────────────────────────────────────────────────────
;
; There are TWO distinct context switch paths:
;
; 1. COOPERATIVE (yield() in sched.c):
;    Uses context_switch(save_esp, load_esp) from context_switch.asm.
;    Saves only callee-saved registers (EBX, ESI, EDI, EBP) + return EIP.
;    The task's saved ESP points to the top of this 5-word frame.
;    Used when a task voluntarily yields.
;
; 2. PREEMPTIVE (IRQ0 handler below):
;    The CPU pushes EFLAGS/CS/EIP automatically.
;    The handler pushes DS, ES, and all GPRs (PUSHA = 8 words).
;    Total interrupt frame = 11 words (44 bytes) on the task's stack.
;    The handler then performs the stack switch ITSELF in assembly,
;    without calling context_switch().
;    The task's saved ESP points to the top of this 11-word frame.
;
; KEY INVARIANT: a task's saved ESP always points to a CONSISTENT frame.
; A task saved by the cooperative path has a 5-word frame.
; A task saved by the preemptive path has an 11-word frame.
;
; When a task is RESUMED:
;   - If it was saved cooperatively: context_switch() pops 4 callee-saved
;     regs and does RET (pops EIP). The task continues after its yield() call.
;   - If it was saved preemptively: irq0_handler pops DS/ES, does POPA,
;     and does IRET (pops EIP/CS/EFLAGS). The task continues at the
;     interrupted instruction.
;
; To distinguish the two cases, we use a per-task flag:
;   g_tasks[id].in_irq (uint8_t, 0 = cooperative, 1 = preemptive)
;
; Actually, the SIMPLEST correct design is to use ONE frame format for
; everything.  We do this by making yield() also go through an interrupt
; (software int 0x30) so both paths use the same 11-word frame.
;
; REVISED DESIGN (simpler and correct):
; ──────────────────────────────────────
; ALL context switches use the SAME frame format: the full interrupt frame
; saved by irq0_handler.  yield() triggers "int 0x30" (software interrupt)
; which goes through yield_handler below.  Both IRQ0 and yield use the
; same save/restore/switch code.
;
; Frame layout on the task's stack (top = lowest address):
;
;   [ESP+0]  DS       ─┐
;   [ESP+4]  ES        │  saved by handler
;   [ESP+8]  EDI       │
;   [ESP+12] ESI       │  (PUSHA order: EAX ECX EDX EBX ESP EBP ESI EDI)
;   [ESP+16] EBP       │  Wait — PUSHA pushes: EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI
;   [ESP+20] (ESP_snap)│  (ESP value at time of PUSHA — not useful, ignored)
;   [ESP+24] EBX       │
;   [ESP+28] EDX       │
;   [ESP+32] ECX       │
;   [ESP+36] EAX      ─┘
;   [ESP+40] EIP      ─┐
;   [ESP+44] CS        │  pushed by CPU on interrupt
;   [ESP+48] EFLAGS   ─┘
;
; Total: 13 words = 52 bytes.
;
; The saved ESP stored in g_tasks[id].esp points to [ESP+0] = DS slot.

bits 32
section .text

global idt_load
global irq0_handler
global irq_spurious_handler
global yield_int_handler

extern sched_tick_preempt   ; void sched_tick_preempt(void) — increments tick, decides preemption
extern sched_next           ; uint32_t sched_next(void) — returns next task index (or current)
extern g_tasks              ; task_t g_tasks[] — scheduler task table
extern g_current            ; uint32_t g_current — index of currently running task
extern g_tick_count         ; uint32_t g_tick_count — ticks since last switch
extern g_ms_ticks           ; volatile uint32_t g_ms_ticks — PIT-based ms counter

; Offsets into task_t struct (must match sched.h)
; task_t { uint32_t esp; uint32_t state; uint8_t *stack_base; uint32_t id; const char *name; uint32_t ticks; }
%define TASK_ESP_OFFSET     0   ; offsetof(task_t, esp)
%define TASK_STATE_OFFSET   4   ; offsetof(task_t, state)
%define TASK_TICKS_OFFSET   20  ; offsetof(task_t, ticks)
%define TASK_SIZE           24  ; sizeof(task_t)

; task_state_t values (must match sched.h)
%define TASK_UNUSED  0
%define TASK_READY   1
%define TASK_RUNNING 2
%define TASK_ZOMBIE  3

; ── idt_load ─────────────────────────────────────────────────────────────────
;
; void idt_load(void *idtr_ptr);
;   Loads the IDT register from the 6-byte IDTR structure at idtr_ptr.

idt_load:
    mov  eax, [esp + 4]     ; eax = idtr_ptr
    lidt [eax]
    ret

; ── irq_spurious_handler ─────────────────────────────────────────────────────
;
; Default handler for all vectors except IRQ0.  Sends EOI and returns.

irq_spurious_handler:
    pusha
    mov  al, 0x20
    out  0x20, al
    popa
    iret

; ── save_and_switch macro ─────────────────────────────────────────────────────
;
; Common code for both IRQ0 and yield_int_handler:
;   1. Registers DS and ES already saved, PUSHA already done.
;   2. ESP now points to the top of the saved frame.
;   3. Save current ESP into g_tasks[g_current].esp
;   4. Find next ready task (call sched_next_asm helper)
;   5. Load next task's ESP
;   6. Restore DS, ES, POPA, IRET

; ── irq0_handler ─────────────────────────────────────────────────────────────
;
; PIT timer interrupt (vector 0x20 = IRQ0).
; Increments the tick counter; if the quantum has expired, switches tasks.

irq0_handler:
    ; --- Save full context ---
    pusha
    push ds
    push es

    ; Load kernel data segment (GRUB sets DS=0x18)
    mov  ax, 0x18
    mov  ds, ax
    mov  es, ax

    ; --- Save current task's ESP ---
    ; g_tasks[g_current].esp = ESP
    mov  eax, [g_current]       ; eax = g_current
    imul eax, TASK_SIZE         ; eax = g_current * sizeof(task_t)
    add  eax, g_tasks           ; eax = &g_tasks[g_current]
    mov  [eax + TASK_ESP_OFFSET], esp   ; save ESP

    ; --- Increment tick counter ---
    inc  dword [g_tick_count]

    ; --- Increment per-task tick counter ---
    inc  dword [eax + TASK_TICKS_OFFSET]

    ; --- Increment PIT-based millisecond counter (10 ms per tick) ---
    add  dword [g_ms_ticks], 10

    ; --- Send EOI to master PIC (must be done before re-enabling interrupts) ---
    mov  al, 0x20
    out  0x20, al

    ; --- Check if quantum expired ---
    ; If g_tick_count < SCHED_QUANTUM_TICKS (5), just restore and return
    cmp  dword [g_tick_count], 5
    jl   .restore

    ; --- Quantum expired: find next ready task ---
    ; Reset tick counter
    mov  dword [g_tick_count], 0

    ; Find next TASK_READY task (round-robin)
    ; We scan g_tasks[(g_current+1) % SCHED_MAX_TASKS ... g_current]
    ; SCHED_MAX_TASKS = 4
    mov  ecx, [g_current]       ; ecx = current task index
    mov  edx, ecx               ; edx = scan index
    mov  esi, 4                 ; esi = SCHED_MAX_TASKS

.scan_loop:
    inc  edx
    ; edx = (edx) % 4
    and  edx, 3                 ; fast modulo for power-of-2
    cmp  edx, ecx               ; wrapped all the way around?
    je   .no_switch             ; no ready task found

    ; Check g_tasks[edx].state
    mov  eax, edx
    imul eax, TASK_SIZE
    add  eax, g_tasks
    cmp  dword [eax + TASK_STATE_OFFSET], TASK_READY
    jne  .scan_loop

    ; Found a ready task at index edx
    ; Mark current task as READY (if it was RUNNING)
    mov  eax, [g_current]
    imul eax, TASK_SIZE
    add  eax, g_tasks
    cmp  dword [eax + TASK_STATE_OFFSET], TASK_RUNNING
    jne  .mark_next
    mov  dword [eax + TASK_STATE_OFFSET], TASK_READY

.mark_next:
    ; Mark next task as RUNNING
    mov  eax, edx
    imul eax, TASK_SIZE
    add  eax, g_tasks
    mov  dword [eax + TASK_STATE_OFFSET], TASK_RUNNING

    ; Update g_current
    mov  [g_current], edx

    ; Load next task's ESP
    mov  esp, [eax + TASK_ESP_OFFSET]
    jmp  .restore

.no_switch:
    ; No other ready task — restore current task's ESP (already saved above)
    ; Just fall through to .restore

.restore:
    ; Restore segment registers and GPRs, then IRET
    pop  es
    pop  ds
    popa
    iret

; ── yield_int_handler ────────────────────────────────────────────────────────
;
; Software interrupt handler for cooperative yield (int 0x30).
; Same frame format as irq0_handler, but always switches to the next task
; (no quantum check).

yield_int_handler:
    ; --- Save full context ---
    pusha
    push ds
    push es

    ; Load kernel data segment
    mov  ax, 0x18
    mov  ds, ax
    mov  es, ax

    ; --- Save current task's ESP ---
    mov  eax, [g_current]
    imul eax, TASK_SIZE
    add  eax, g_tasks
    mov  [eax + TASK_ESP_OFFSET], esp

    ; --- Find next TASK_READY task ---
    mov  ecx, [g_current]
    mov  edx, ecx

.yield_scan:
    inc  edx
    and  edx, 3
    cmp  edx, ecx
    je   .yield_no_switch

    mov  eax, edx
    imul eax, TASK_SIZE
    add  eax, g_tasks
    cmp  dword [eax + TASK_STATE_OFFSET], TASK_READY
    jne  .yield_scan

    ; Found next task
    mov  eax, [g_current]
    imul eax, TASK_SIZE
    add  eax, g_tasks
    cmp  dword [eax + TASK_STATE_OFFSET], TASK_RUNNING
    jne  .yield_mark_next
    mov  dword [eax + TASK_STATE_OFFSET], TASK_READY

.yield_mark_next:
    mov  eax, edx
    imul eax, TASK_SIZE
    add  eax, g_tasks
    mov  dword [eax + TASK_STATE_OFFSET], TASK_RUNNING
    mov  [g_current], edx
    mov  dword [g_tick_count], 0
    mov  esp, [eax + TASK_ESP_OFFSET]

.yield_no_switch:
    pop  es
    pop  ds
    popa
    iret
