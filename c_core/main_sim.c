/*
 * main_sim.c
 * ──────────
 * Driver that exercises every Part-2 module:
 *   1. Process manager  (edu_fork, edu_exec, edu_wait, edu_exit, edu_ps)
 *   2. Thread pool      (submit tasks, graceful shutdown)
 *   3. Threading models (Many-to-One, One-to-One)
 *   4. Race demo        (controlled by compile flag)
 *   5. Producer-consumer semaphore demo
 *   6. IPC: shared memory + anonymous pipe
 *
 * The Python controller (main_controller.py) reads stdout and
 * pcb_snapshot.json produced by this binary.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "include/eduos.h"

/* ─── Simple task for thread pool demo ───────────────────────────── */
static void pool_task(void *arg) {
    int id = *(int *)arg;
    printf("[PoolTask] Task %d running on thread pool worker\n", id);
    fflush(stdout);
    free(arg);
}

/* ─── Helper: create a sample PCB ────────────────────────────────── */
static PCB make_pcb(pid_t pid, const char *name, int priority,
                    int burst, int arrival, int mem_kb, int owner_id) {
    PCB p;
    memset(&p, 0, sizeof(p));
    p.pid            = pid;
    p.parent_pid     = -1;
    strncpy(p.name, name, sizeof(p.name) - 1);
    p.state          = STATE_READY;
    p.priority       = priority;
    p.burst_time     = burst;
    p.arrival_time   = arrival;
    p.remaining_time = burst;
    p.memory_req_kb  = mem_kb;
    p.thread_count   = 1;
    p.creation_time  = time(NULL);
    p.owner_id       = owner_id;
    p.is_active      = 1;
    return p;
}

int main(void) {
    printf("╔══════════════════════════════════════╗\n");
    printf("║        EduOS Simulator v1.0          ║\n");
    printf("╚══════════════════════════════════════╝\n\n");
    fflush(stdout);

    /* ── 1. Process Manager ── */
    printf("━━━ Part 1: Process Manager ━━━\n");
    init_process_manager();

    /* Seed the PCB table with a root process (pid=1, like Linux init) */
    PCB root = make_pcb(1, "init", 0, 20, 0, 512, 0);
    root.parent_pid = 0;   /* 0 = no parent */
    pcb_table[0] = root;
    pcb_count    = 1;
    next_pid     = 2;   /* reserve 1 for init */

    /* Fork two children from init */
    pid_t child1 = edu_fork(&pcb_table[0]);
    pid_t child2 = edu_fork(&pcb_table[0]);

    /* Exec child1 to replace its program image */
    edu_exec(child1, "browser", 15);
    edu_exec(child2, "editor",  10);

    /* Show the process table */
    edu_ps();

    /* Simulate child1 running and exiting */
    PCB *c1 = find_pcb(child1);
    if (c1) { c1->state = STATE_RUNNING; }
    edu_exit(child1, 0);

    /* Simulate child2 running and exiting */
    PCB *c2 = find_pcb(child2);
    if (c2) { c2->state = STATE_RUNNING; }
    edu_exit(child2, 42);

    /* Parent waits for all children */
    int last_exit = edu_wait(1);
    printf("edu_wait returned exit code: %d\n\n", last_exit);

    edu_ps();

    /* ── 2. Thread Pool ── */
    printf("━━━ Part 2: Thread Pool ━━━\n");
    thread_pool_init();

    for (int i = 0; i < 8; i++) {
        int *id = malloc(sizeof(int));
        if (!id) { perror("malloc"); continue; }
        *id = i;
        thread_pool_submit(pool_task, id);
    }

    thread_pool_shutdown();
    printf("\n");

    /* ── 3. Threading Models ── */
    printf("━━━ Part 3: Threading Models ━━━\n");
    demo_many_to_one();
    demo_one_to_one();

    /* ── 4. Race Condition ── */
    printf("━━━ Part 4: Race Condition Demo ━━━\n");
    demo_race_condition();
    printf("\n");

    /* ── 5. Producer-Consumer ── */
    printf("━━━ Part 5: Producer-Consumer (Semaphores) ━━━\n");
    demo_producer_consumer();

    /* ── 6. Deadlock ── */
    printf("━━━ Part 6: Deadlock Demo ━━━\n");
    demo_deadlock();

    /* ── 7. IPC: Shared Memory ── */
    printf("━━━ Part 7: IPC — Shared Memory ━━━\n");
    PCB shm_proc = make_pcb(10, "shm_writer", 1, 8, 0, 256, 42);
    shm_proc.state = STATE_RUNNING;
    ipc_shared_memory_demo(&shm_proc, 42);   /* matching owner_id */

    /* Demonstrate access denial */
    printf("-- Access Denial Test --\n");
    PCB denied = make_pcb(11, "intruder", 3, 5, 0, 128, 99); /* wrong owner */
    denied.state = STATE_RUNNING;
    ipc_shared_memory_demo(&denied, 42);   /* region owned by 42, proc is 99 */

    /* ── 8. IPC: Anonymous Pipe ── */
    printf("━━━ Part 8: IPC — Anonymous Pipe ━━━\n");
    PCB pipe_procs[3];
    pipe_procs[0] = make_pcb(20, "proc_alpha",  2, 10, 0, 200, 1);
    pipe_procs[1] = make_pcb(21, "proc_beta",   1,  5, 2, 100, 1);
    pipe_procs[2] = make_pcb(22, "proc_gamma",  3,  8, 4, 150, 1);
    pipe_procs[0].state = STATE_READY;
    pipe_procs[1].state = STATE_RUNNING;
    pipe_procs[2].state = STATE_WAITING;

    ipc_pipe_demo(pipe_procs, 3);

    printf("╔══════════════════════════════════════╗\n");
    printf("║        EduOS Simulation Complete     ║\n");
    printf("╚══════════════════════════════════════╝\n");
    fflush(stdout);

    return 0;
}
