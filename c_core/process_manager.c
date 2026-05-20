/*
 * process_manager.c
 * ─────────────────
 * Implements the EduOS Process Control Block table and the four
 * system-call-like functions: edu_fork, edu_exec, edu_wait, edu_exit.
 * Also provides edu_ps() (like `ps aux`) and a hand-rolled JSON
 * serialiser that writes pcb_snapshot.json on every state change.
 *
 * Design note: a global mutex (pcb_mutex) protects every access to
 * pcb_table, mirroring how a real kernel serialises process-table
 * updates via a spinlock or rwlock.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>

#include "include/eduos.h"

/* ─── Globals ────────────────────────────────────────────────────── */
PCB            pcb_table[MAX_PROCESSES];
int            pcb_count = 0;
pthread_mutex_t pcb_mutex = PTHREAD_MUTEX_INITIALIZER;

pid_t   next_pid   = 1;   /* monotonic PID counter            */

/* ─── Helpers ────────────────────────────────────────────────────── */

/* Return a human-readable state name */
const char *state_name(int state) {
    switch (state) {
        case STATE_NEW:        return "NEW";
        case STATE_READY:      return "READY";
        case STATE_RUNNING:    return "RUNNING";
        case STATE_WAITING:    return "WAITING";
        case STATE_TERMINATED: return "TERMINATED";
        default:               return "UNKNOWN";
    }
}

/* Timestamped log line — mirrors kernel printk() */
static void log_event(const char *fmt, ...) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        perror("clock_gettime");
        return;
    }
    /* Print seconds.milliseconds prefix */
    fprintf(stdout, "[%ld.%03ld] ", (long)ts.tv_sec,
            (long)(ts.tv_nsec / 1000000));
    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    fprintf(stdout, "\n");
    fflush(stdout);
}

/* Find a PCB by PID — caller must hold pcb_mutex */
PCB *find_pcb(pid_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (pcb_table[i].is_active && pcb_table[i].pid == pid)
            return &pcb_table[i];
    }
    return NULL;
}

/* Find a free slot in the PCB table — caller must hold pcb_mutex */
static PCB *alloc_pcb_slot(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!pcb_table[i].is_active)
            return &pcb_table[i];
    }
    return NULL;
}

/* ─── JSON Serialiser (no external library) ───────────────────────── */
/*
 * Escapes a C string for safe embedding in JSON:
 * replaces backslash, double-quote, and ASCII control chars.
 */
static void json_escape(char *dst, size_t dst_sz, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dst_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= dst_sz) break;
            dst[j++] = '\\';
            dst[j++] = (char)c;
        } else if (c < 0x20) {
            /* encode control chars as \uXXXX */
            if (j + 6 >= dst_sz) break;
            snprintf(dst + j, dst_sz - j, "\\u%04x", c);
            j += 6;
        } else {
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
}

void serialize_pcb_to_json(const char *filename) {
    /* Lock is assumed to be held by caller */
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        perror("fopen pcb_snapshot.json");
        return;
    }

    char escaped[128];
    fprintf(fp, "[\n");
    int first = 1;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!pcb_table[i].is_active) continue;
        PCB *p = &pcb_table[i];

        if (!first) fprintf(fp, ",\n");
        first = 0;

        json_escape(escaped, sizeof(escaped), p->name);

        fprintf(fp,
            "  {\n"
            "    \"pid\":            %d,\n"
            "    \"name\":           \"%s\",\n"
            "    \"state\":          \"%s\",\n"
            "    \"state_code\":     %d,\n"
            "    \"priority\":       %d,\n"
            "    \"burst_time\":     %d,\n"
            "    \"arrival_time\":   %d,\n"
            "    \"remaining_time\": %d,\n"
            "    \"memory_req_kb\":  %d,\n"
            "    \"thread_count\":   %d,\n"
            "    \"creation_time\":  %ld,\n"
            "    \"parent_pid\":     %d,\n"
            "    \"exit_code\":      %d,\n"
            "    \"owner_id\":       %d\n"
            "  }",
            p->pid, escaped, state_name(p->state), p->state,
            p->priority, p->burst_time, p->arrival_time,
            p->remaining_time, p->memory_req_kb, p->thread_count,
            (long)p->creation_time, p->parent_pid,
            p->exit_code, p->owner_id);
    }

    fprintf(fp, "\n]\n");
    if (fclose(fp) != 0)
        perror("fclose pcb_snapshot.json");
}

/* ─── Public API ─────────────────────────────────────────────────── */

void init_process_manager(void) {
    memset(pcb_table, 0, sizeof(pcb_table));
    pcb_count = 0;
    next_pid  = 1;
    log_event("process_manager: initialised (max %d slots)", MAX_PROCESSES);
}

/*
 * edu_fork — creates a child PCB by copying the parent.
 *
 * Real fork(2): the kernel duplicates the parent's address space
 * (copy-on-write), file descriptor table, and signal mask.
 * EduOS simplifies this to copying the PCB struct only.
 */
pid_t edu_fork(PCB *parent) {
    if (!parent) {
        fprintf(stderr, "edu_fork: NULL parent\n");
        return -1;
    }

    if (pthread_mutex_lock(&pcb_mutex) != 0) {
        perror("edu_fork: pthread_mutex_lock");
        return -1;
    }

    PCB *child = alloc_pcb_slot();
    if (!child) {
        fprintf(stderr, "edu_fork: PCB table full\n");
        pthread_mutex_unlock(&pcb_mutex);
        return -1;
    }

    /* Copy parent → child, then override child-specific fields */
    memcpy(child, parent, sizeof(PCB));
    child->pid          = next_pid++;
    child->parent_pid   = parent->pid;
    child->state        = STATE_NEW;
    child->exit_code    = 0;
    child->is_active    = 1;
    child->creation_time = time(NULL);
    if (child->creation_time == (time_t)-1)
        perror("edu_fork: time");

    pcb_count++;
    log_event("edu_fork: child PID %d forked from parent PID %d (state→NEW)",
              child->pid, parent->pid);

    /* Transition to READY immediately (scheduler picks it up) */
    child->state = STATE_READY;
    log_event("edu_fork: PID %d state→READY", child->pid);

    serialize_pcb_to_json("pcb_snapshot.json");

    pid_t new_pid = child->pid;
    if (pthread_mutex_unlock(&pcb_mutex) != 0)
        perror("edu_fork: pthread_mutex_unlock");

    return new_pid;
}

/*
 * edu_exec — replaces a process's program image.
 *
 * Real execve(2): the kernel discards the current address space and
 * loads a new ELF binary, resetting the stack, heap, and BSS.
 * EduOS simplifies this to updating the name and burst times.
 */
void edu_exec(pid_t pid, char *prog_name, int burst_time) {
    if (!prog_name) {
        fprintf(stderr, "edu_exec: NULL prog_name\n");
        return;
    }
    if (burst_time <= 0) {
        fprintf(stderr, "edu_exec: burst_time must be > 0\n");
        return;
    }

    if (pthread_mutex_lock(&pcb_mutex) != 0) {
        perror("edu_exec: pthread_mutex_lock");
        return;
    }

    PCB *p = find_pcb(pid);
    if (!p) {
        fprintf(stderr, "edu_exec: PID %d not found\n", pid);
        pthread_mutex_unlock(&pcb_mutex);
        return;
    }
    if (p->state == STATE_TERMINATED) {
        fprintf(stderr, "edu_exec: PID %d already terminated\n", pid);
        pthread_mutex_unlock(&pcb_mutex);
        return;
    }

    strncpy(p->name, prog_name, sizeof(p->name) - 1);
    p->name[sizeof(p->name) - 1] = '\0';
    p->burst_time     = burst_time;
    p->remaining_time = burst_time;

    log_event("edu_exec: PID %d image replaced → \"%s\" (burst=%d)",
              pid, p->name, burst_time);

    serialize_pcb_to_json("pcb_snapshot.json");

    if (pthread_mutex_unlock(&pcb_mutex) != 0)
        perror("edu_exec: pthread_mutex_unlock");
}

/*
 * edu_wait — blocks until all children of parent_pid are TERMINATED.
 *
 * Real wait(2): the kernel suspends the calling process and wakes it
 * when a child sends SIGCHLD.  EduOS polls with a small sleep.
 */
int edu_wait(pid_t parent_pid) {
    log_event("edu_wait: PID %d waiting for children", parent_pid);

    /* Mark parent as WAITING */
    if (pthread_mutex_lock(&pcb_mutex) != 0) {
        perror("edu_wait: pthread_mutex_lock");
        return -1;
    }
    PCB *parent = find_pcb(parent_pid);
    if (parent) parent->state = STATE_WAITING;
    serialize_pcb_to_json("pcb_snapshot.json");
    if (pthread_mutex_unlock(&pcb_mutex) != 0)
        perror("edu_wait: pthread_mutex_unlock");

    int last_exit = 0;

    /* Poll until no active non-terminated children remain */
    for (;;) {
        if (pthread_mutex_lock(&pcb_mutex) != 0) {
            perror("edu_wait: pthread_mutex_lock (poll)");
            break;
        }

        int pending = 0;
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (!pcb_table[i].is_active) continue;
            if (pcb_table[i].parent_pid != parent_pid) continue;
            if (pcb_table[i].state != STATE_TERMINATED) {
                pending++;
            } else {
                last_exit = pcb_table[i].exit_code;
            }
        }

        if (pthread_mutex_unlock(&pcb_mutex) != 0)
            perror("edu_wait: pthread_mutex_unlock (poll)");

        if (pending == 0) break;

        /* Yield CPU — real kernel would block on a wait queue */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 }; /* 50 ms */
        nanosleep(&ts, NULL);
    }

    /* Restore parent to RUNNING */
    if (pthread_mutex_lock(&pcb_mutex) != 0)
        perror("edu_wait: pthread_mutex_lock (restore)");
    PCB *p2 = find_pcb(parent_pid);
    if (p2 && p2->state == STATE_WAITING) p2->state = STATE_RUNNING;
    serialize_pcb_to_json("pcb_snapshot.json");
    if (pthread_mutex_unlock(&pcb_mutex) != 0)
        perror("edu_wait: pthread_mutex_unlock (restore)");

    log_event("edu_wait: PID %d all children terminated (last exit=%d)",
              parent_pid, last_exit);
    return last_exit;
}

/*
 * edu_exit — terminates a process and frees its PCB slot.
 *
 * Real _exit(2): the kernel reclaims the process's memory pages,
 * closes open file descriptors, and sends SIGCHLD to the parent.
 * EduOS marks the slot terminated; slot is reusable after edu_wait.
 */
void edu_exit(pid_t pid, int exit_code) {
    if (pthread_mutex_lock(&pcb_mutex) != 0) {
        perror("edu_exit: pthread_mutex_lock");
        return;
    }

    PCB *p = find_pcb(pid);
    if (!p) {
        fprintf(stderr, "edu_exit: PID %d not found\n", pid);
        pthread_mutex_unlock(&pcb_mutex);
        return;
    }

    p->state     = STATE_TERMINATED;
    p->exit_code = exit_code;
    /* Keep is_active=1 so edu_wait can read the exit code;
     * real kernel keeps a zombie until wait() is called.      */

    log_event("edu_exit: PID %d terminated (exit_code=%d)", pid, exit_code);

    serialize_pcb_to_json("pcb_snapshot.json");

    if (pthread_mutex_unlock(&pcb_mutex) != 0)
        perror("edu_exit: pthread_mutex_unlock");
}

/*
 * edu_ps — prints a formatted table of all active PCBs.
 * Mirrors the output of `ps aux` on Linux.
 */
void edu_ps(void) {
    if (pthread_mutex_lock(&pcb_mutex) != 0) {
        perror("edu_ps: pthread_mutex_lock");
        return;
    }

    printf("\n%-6s %-6s %-12s %-11s %5s %5s %5s %7s %7s\n",
           "PID", "PPID", "NAME", "STATE",
           "PRIO", "BURST", "REM", "MEM_KB", "THDS");
    printf("%-6s %-6s %-12s %-11s %5s %5s %5s %7s %7s\n",
           "------", "------", "------------", "-----------",
           "-----", "-----", "-----", "-------", "-------");

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!pcb_table[i].is_active) continue;
        PCB *p = &pcb_table[i];
        printf("%-6d %-6d %-12s %-11s %5d %5d %5d %7d %7d\n",
               p->pid, p->parent_pid, p->name,
               state_name(p->state),
               p->priority, p->burst_time,
               p->remaining_time, p->memory_req_kb,
               p->thread_count);
    }
    printf("\n");
    fflush(stdout);

    if (pthread_mutex_unlock(&pcb_mutex) != 0)
        perror("edu_ps: pthread_mutex_unlock");
}
