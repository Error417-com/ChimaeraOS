/*
 * ChimaeraOS — Round-Robin Scheduler
 * src/proc/sched.c
 *
 * Implements a simple preemptive round-robin scheduler for up to
 * SCHED_MAX_TASKS (4) tasks, including the idle/main task (task 0).
 *
 * Context switch design
 * ---------------------
 * ALL context switches — both cooperative (yield) and preemptive (IRQ0) —
 * use the SAME interrupt frame format.  This avoids the frame-mismatch bug
 * that arises when cooperative and preemptive paths save different amounts
 * of state.
 *
 * The frame layout on each task's kernel stack (top = lowest address):
 *
 *   [ESP+0]  DS        ─┐
 *   [ESP+4]  ES         │  saved by handler
 *   [ESP+8]  EDI        │
 *   [ESP+12] ESI        │
 *   [ESP+16] EBP        │  PUSHA order (high to low):
 *   [ESP+20] (ESP snap) │    EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI
 *   [ESP+24] EBX        │  (ESP snap is the ESP *before* PUSHA — not restored)
 *   [ESP+28] EDX        │
 *   [ESP+32] ECX        │
 *   [ESP+36] EAX       ─┘
 *   [ESP+40] EIP       ─┐
 *   [ESP+44] CS         │  pushed by CPU on interrupt entry
 *   [ESP+48] EFLAGS    ─┘
 *
 * Total: 13 words = 52 bytes.
 *
 * The cooperative yield() calls "int 0x30" (software interrupt), which
 * goes through yield_int_handler in idt.asm.  The CPU pushes EFLAGS/CS/EIP
 * automatically, then the handler pushes DS/ES/PUSHA — same as IRQ0.
 *
 * New task initialisation
 * -----------------------
 * sched_spawn() pre-builds this 13-word frame on the new task's stack so
 * that the first time the task is resumed (via IRET), it starts executing
 * at task_entry_trampoline with interrupts enabled.
 *
 * The entry function pointer is passed in EBX (word at [ESP+24] in the
 * frame above).  task_entry_trampoline reads EBX and calls the entry fn.
 *
 * Globals exported to idt.asm
 * ---------------------------
 * g_tasks[], g_current, g_tick_count are declared extern in idt.asm and
 * manipulated directly in assembly for maximum efficiency.
 */

#include "../include/sched.h"
#include "../include/mm.h"
#include "../include/serial.h"
#include "../include/types.h"

/* Defined in src/proc/context_switch.asm */
extern void task_entry_trampoline(void);

/* Defined in src/kernel/idt.asm */
extern void yield_int_handler(void);

/* ── Quantum ─────────────────────────────────────────────────────────────── */

/* Number of PIT ticks before forced preemption (~10 ms per tick → 50 ms) */
#define SCHED_QUANTUM_TICKS  5u

/* ── Scheduler state (exported to idt.asm) ───────────────────────────────── */

task_t   g_tasks[SCHED_MAX_TASKS];
uint32_t g_current    = 0;
uint32_t g_tick_count = 0;
uint32_t g_ntasks     = 0;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static inline void _cli(void)  { __asm__ volatile ("cli"); }
static inline void _sti(void)  { __asm__ volatile ("sti"); }
static inline uint32_t _flags(void)
{
    uint32_t f;
    __asm__ volatile ("pushf; pop %0" : "=r"(f));
    return f;
}

/* ── task_trampoline_c ───────────────────────────────────────────────────── */

/*
 * Called from task_entry_trampoline (asm) with the entry function pointer
 * as the first cdecl argument.  Enables interrupts, calls entry(), then
 * marks the task as TASK_ZOMBIE and yields forever.
 */
void __attribute__((noreturn)) task_trampoline_c(void (*entry)(void))
{
    /* Enable interrupts — the task runs with interrupts enabled */
    _sti();

    /* Call the actual task function */
    entry();

    /* Task returned — mark as zombie and yield forever */
    _cli();
    g_tasks[g_current].state = TASK_ZOMBIE;
    serial_puts("[SCHED] Task ");
    serial_putchar('0' + (char)g_current);
    serial_puts(" (");
    serial_puts(g_tasks[g_current].name ? g_tasks[g_current].name : "?");
    serial_puts(") exited\n");
    _sti();

    for (;;) {
        __asm__ volatile ("int $0x30");  /* cooperative yield */
    }
    __builtin_unreachable();
}

/* ── sched_init ──────────────────────────────────────────────────────────── */

void sched_init(void)
{
    /* Zero all task slots */
    for (uint32_t i = 0; i < SCHED_MAX_TASKS; i++) {
        g_tasks[i].esp        = 0;
        g_tasks[i].state      = TASK_UNUSED;
        g_tasks[i].stack_base = NULL;
        g_tasks[i].id         = i;
        g_tasks[i].name       = NULL;
        g_tasks[i].ticks      = 0;
    }

    /* Register the current execution context as task 0 (idle/main task) */
    g_tasks[0].state = TASK_RUNNING;
    g_tasks[0].name  = "main";
    g_current        = 0;
    g_ntasks         = 1;

    serial_puts("[SCHED] Scheduler initialised (task 0 = main)\n");
}

/* ── sched_spawn ─────────────────────────────────────────────────────────── */

/*
 * Pre-built interrupt frame for a new task.
 *
 * When the task is first resumed via IRET, the CPU pops:
 *   EIP   = task_entry_trampoline
 *   CS    = 0x10 (kernel code segment)
 *   EFLAGS = 0x0202 (IF=1, reserved bit 1 set)
 *
 * Then the handler has already done POPA which restores:
 *   EAX=0, ECX=0, EDX=0, EBX=entry_fn, (ESP ignored), EBP=0, ESI=0, EDI=0
 *
 * Then DS and ES are popped (both = 0x18).
 *
 * Frame layout (13 words, top = lowest address):
 *
 *   [0]  DS    = 0x18
 *   [1]  ES    = 0x18
 *   [2]  EDI   = 0
 *   [3]  ESI   = 0
 *   [4]  EBP   = 0
 *   [5]  ESP_snap = 0  (ignored by POPA)
 *   [6]  EBX   = entry_fn  ← entry function pointer
 *   [7]  EDX   = 0
 *   [8]  ECX   = 0
 *   [9]  EAX   = 0
 *   [10] EIP   = task_entry_trampoline
 *   [11] CS    = 0x10
 *   [12] EFLAGS = 0x0202  (IF=1)
 *
 * PUSHA pushes in this order (high to low): EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI
 * POPA  pops  in this order (low to high):  EDI, ESI, EBP, (skip ESP), EBX, EDX, ECX, EAX
 *
 * So the frame (from lowest address = top of stack) is:
 *   DS, ES, EDI, ESI, EBP, ESP_snap, EBX, EDX, ECX, EAX, EIP, CS, EFLAGS
 *
 * Wait — let me re-derive this carefully.
 *
 * irq0_handler / yield_int_handler does:
 *   pusha          ; pushes EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI (8 words, EAX first = highest)
 *   push ds        ; DS
 *   push es        ; ES  ← ESP now points here (lowest address)
 *
 * So the frame from lowest address (top of stack) upward is:
 *   [ESP+0]  ES
 *   [ESP+4]  DS
 *   [ESP+8]  EDI   ← PUSHA pushed EDI last (lowest address in PUSHA block)
 *   [ESP+12] ESI
 *   [ESP+16] EBP
 *   [ESP+20] ESP_snap
 *   [ESP+24] EBX
 *   [ESP+28] EDX
 *   [ESP+32] ECX
 *   [ESP+36] EAX   ← PUSHA pushed EAX first (highest address in PUSHA block)
 *   [ESP+40] EIP   ← CPU pushed on interrupt
 *   [ESP+44] CS
 *   [ESP+48] EFLAGS
 *
 * Restore order:
 *   pop es         ; [ESP+0]
 *   pop ds         ; [ESP+4]
 *   popa           ; pops EDI,ESI,EBP,(skip ESP),EBX,EDX,ECX,EAX
 *   iret           ; pops EIP,CS,EFLAGS
 */

int sched_spawn(void (*entry)(void), const char *name)
{
    /* Find a free slot */
    uint32_t slot = 0;
    for (uint32_t i = 1; i < SCHED_MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_UNUSED) { slot = i; break; }
    }
    if (slot == 0) {
        serial_puts("[SCHED] sched_spawn: no free slots\n");
        return -1;
    }

    /* Allocate a kernel stack */
    uint8_t *stack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!stack) {
        serial_puts("[SCHED] sched_spawn: mm_alloc failed\n");
        return -1;
    }

    /*
     * Build the initial interrupt frame on the task's stack.
     * sp starts at stack_top and grows downward.
     *
     * Frame (from lowest address = top of stack):
     *   ES, DS, EDI, ESI, EBP, ESP_snap, EBX=entry, EDX, ECX, EAX, EIP, CS, EFLAGS
     */
    uint32_t *sp = (uint32_t *)(stack + TASK_STACK_SIZE);

    /* CPU interrupt frame (pushed last = lowest on stack after handler saves regs) */
    *(--sp) = 0x00000202u;                      /* EFLAGS: IF=1, reserved=1 */
    *(--sp) = 0x10u;                             /* CS = kernel code segment */
    *(--sp) = (uint32_t)task_entry_trampoline;  /* EIP */

    /* PUSHA frame (EAX pushed first = highest address, EDI pushed last = lowest) */
    /* PUSHA order: EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI */
    /* So on stack (lowest to highest): EDI, ESI, EBP, ESP_snap, EBX, EDX, ECX, EAX */
    *(--sp) = 0u;                /* EAX */
    *(--sp) = 0u;                /* ECX */
    *(--sp) = 0u;                /* EDX */
    *(--sp) = (uint32_t)entry;   /* EBX = entry function pointer */
    *(--sp) = 0u;                /* ESP_snap (ignored by POPA) */
    *(--sp) = 0u;                /* EBP */
    *(--sp) = 0u;                /* ESI */
    *(--sp) = 0u;                /* EDI */

    /* Segment registers saved by handler */
    *(--sp) = 0x18u;             /* DS */
    *(--sp) = 0x18u;             /* ES  ← ESP points here */

    /* The saved ESP points to the ES slot (top of the saved frame) */
    g_tasks[slot].esp        = (uint32_t)sp;
    g_tasks[slot].state      = TASK_READY;
    g_tasks[slot].stack_base = stack;
    g_tasks[slot].id         = slot;
    g_tasks[slot].name       = name;
    g_tasks[slot].ticks      = 0;

    g_ntasks++;

    serial_puts("[SCHED] Spawned task ");
    serial_putchar('0' + (char)slot);
    serial_puts(" (");
    serial_puts(name ? name : "?");
    serial_puts(")\n");

    return (int)slot;
}

/* ── yield ───────────────────────────────────────────────────────────────── */

void yield(void)
{
    /*
     * Trigger a software interrupt (int 0x30) which goes through
     * yield_int_handler in idt.asm.  The CPU pushes EFLAGS/CS/EIP,
     * then the handler saves DS/ES/PUSHA — same frame as IRQ0.
     *
     * yield_int_handler finds the next TASK_READY task and switches to it.
     * When this task is rescheduled, IRET resumes here (after the int $0x30).
     */
    __asm__ volatile ("int $0x30");
}

/* ── sched_tick (called from IRQ0 handler in idt.asm) ───────────────────── */

/*
 * NOTE: sched_tick() is no longer called from C.  The tick counting and
 * preemption logic is now entirely in irq0_handler (idt.asm) for correctness.
 * This stub is kept for potential future use.
 */
void sched_tick(void)
{
    /* No-op: preemption is handled in idt.asm irq0_handler */
}

/* ── sched_current ───────────────────────────────────────────────────────── */

task_t *sched_current(void)
{
    return &g_tasks[g_current];
}

/* ── sched_start ─────────────────────────────────────────────────────────── */

void sched_start(void)
{
    serial_puts("[SCHED] Starting scheduler (enabling interrupts)\n");

    /* Mark task 0 as READY so it participates in round-robin */
    g_tasks[0].state = TASK_RUNNING;

    _sti();

    /* Spin until all non-idle tasks are zombies */
    for (;;) {
        bool all_done = true;
        for (uint32_t i = 1; i < SCHED_MAX_TASKS; i++) {
            if (g_tasks[i].state == TASK_READY ||
                g_tasks[i].state == TASK_RUNNING) {
                all_done = false;
                break;
            }
        }
        if (all_done) break;

        /* Cooperative yield to let other tasks run */
        yield();

        /* Halt until the next interrupt to avoid burning CPU */
        __asm__ volatile ("hlt");
    }

    _cli();
    serial_puts("[SCHED] All tasks complete\n");
}
