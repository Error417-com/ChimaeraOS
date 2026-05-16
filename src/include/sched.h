/*
 * ChimaeraOS — Preemptive Round-Robin Scheduler
 * src/include/sched.h
 *
 * Public API for the kernel scheduler.
 *
 * Design
 * ------
 * All tasks run in ring 0 (kernel mode only).  Each task has its own
 * kernel stack allocated from the heap.  The saved context is just the
 * kernel stack pointer — all other registers are on the stack.
 *
 * Context switch ABI (cooperative path, context_switch.asm)
 * ----------------------------------------------------------
 *   void context_switch(uint32_t *save_esp, uint32_t load_esp);
 *
 *   Saves EBX, ESI, EDI, EBP, and the return address (EIP) onto the
 *   current stack, writes ESP to *save_esp, loads load_esp into ESP,
 *   then pops EBX/ESI/EDI/EBP and returns — resuming the next task at
 *   the point it last called context_switch (or at its entry point if
 *   newly spawned).
 *
 * Preemptive path (irq0_handler in idt.asm)
 * ------------------------------------------
 *   The PIT IRQ0 handler saves ALL registers (pusha + segment regs),
 *   calls sched_tick(), which may call context_switch().  On return the
 *   handler restores registers and issues iret.
 *
 * Task states
 * -----------
 *   TASK_READY   — runnable, in the run queue
 *   TASK_RUNNING — currently executing
 *   TASK_ZOMBIE  — returned from entry function; will be reaped
 *
 * Limits
 * ------
 *   SCHED_MAX_TASKS  4   (MVP: no dynamic allocation of task slots)
 *   TASK_STACK_SIZE  16 KiB per task
 */

#ifndef SCHED_H
#define SCHED_H

#include "types.h"

/* ── Constants ───────────────────────────────────────────────────────────── */

#define SCHED_MAX_TASKS   4
#define TASK_STACK_SIZE   (16 * 1024)   /* 16 KiB per task */

/* ── Task states ─────────────────────────────────────────────────────────── */

typedef enum {
    TASK_UNUSED  = 0,
    TASK_READY   = 1,
    TASK_RUNNING = 2,
    TASK_ZOMBIE  = 3,
} task_state_t;

/* ── task_t ──────────────────────────────────────────────────────────────── */

typedef struct task {
    uint32_t      esp;          /* saved kernel stack pointer                */
    task_state_t  state;        /* current state                             */
    uint8_t      *stack_base;   /* bottom of the allocated stack (for free)  */
    uint32_t      id;           /* task ID (0 = idle/main)                   */
    const char   *name;         /* human-readable name (for debug output)    */
    uint32_t      ticks;        /* number of timer ticks consumed            */
} task_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/*
 * sched_init() — must be called before any other scheduler function.
 * Registers the current execution context as task 0 (the idle/main task).
 * Does NOT enable interrupts — call sched_start() for that.
 */
void sched_init(void);

/*
 * sched_spawn(entry, name) — create a new TASK_READY task.
 * Returns the task ID (1–3), or -1 if no slot is available.
 * The task begins executing at entry() with interrupts enabled.
 */
int sched_spawn(void (*entry)(void), const char *name);

/*
 * sched_start() — enable interrupts and enter the scheduler loop.
 * The calling context becomes task 0.  Returns only when all tasks
 * other than task 0 have become zombies.
 */
void sched_start(void);

/*
 * yield() — cooperatively relinquish the CPU to the next TASK_READY task.
 * Safe to call with interrupts enabled or disabled.
 */
void yield(void);

/*
 * sched_tick() — called from the IRQ0 handler on each timer tick.
 * Increments the current task's tick counter and calls yield() if the
 * task has consumed its quantum (SCHED_QUANTUM_TICKS ticks).
 * Must be called with interrupts disabled (we are inside an IRQ handler).
 */
void sched_tick(void);

/*
 * sched_current() — return a pointer to the currently running task_t.
 */
task_t *sched_current(void);

/*
 * context_switch(save_esp, load_esp) — low-level context switch.
 * Implemented in src/proc/context_switch.asm.
 * Saves callee-saved registers + EIP onto the current stack,
 * writes ESP to *save_esp, loads load_esp, restores registers, returns.
 */
void context_switch(uint32_t *save_esp, uint32_t load_esp);

/*
 * task_trampoline_c(entry) — C-level task entry trampoline.
 * Called from task_entry_trampoline (asm) with the entry function pointer
 * as the first cdecl argument.  Enables interrupts, calls entry(), then
 * marks the task as TASK_ZOMBIE and yields forever.
 */
void __attribute__((noreturn)) task_trampoline_c(void (*entry)(void));

#endif /* SCHED_H */
