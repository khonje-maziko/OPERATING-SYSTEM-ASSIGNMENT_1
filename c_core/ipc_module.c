/*
 * ipc_module.c
 * ────────────
 * Implements two IPC mechanisms required by Part 2.3:
 *
 * 1. POSIX Shared Memory (shm_open + mmap)
 *    Two processes write/read a SharedMetrics struct in a named
 *    shared-memory region.  A pthread_mutex embedded in the struct
 *    coordinates concurrent access.
 *    Access-control: a process may only read/write if its PCB
 *    owner_id matches the region's owner_id — mirroring OS
 *    protection rings (Ring 0 = kernel, Ring 3 = user process).
 *
 * 2. Anonymous Pipe (pipe())
 *    A parent process serialises PCB data and writes it to a pipe.
 *    A forked child reads and prints the data, then exits.
 *    This mirrors how shell pipelines (ls | grep) work at the
 *    kernel level.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <pthread.h>
#include <time.h>

#include "include/eduos.h"

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 1: POSIX Shared Memory
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * Access-control check:
 *   Mirrors OS protection rings — a process at "user level" (owner_id != 0)
 *   may only access shared memory it owns.  Kernel-level (owner_id == 0)
 *   can access any region (like Ring 0 vs Ring 3).
 *
 *   Relation to real OS:
 *   On Linux, protection rings are enforced by the CPU's CPL (Current
 *   Privilege Level) bits.  shm_open uses file permissions for the same
 *   purpose.  Our owner_id field is an application-layer analogue.
 */
static int access_allowed(SharedMetrics *shm, int requester_owner_id) {
    /* owner_id 0 = "kernel" — always allowed (Ring 0 privilege) */
    if (requester_owner_id == 0) return 1;
    return (shm->owner_id == requester_owner_id);
}

void ipc_shared_memory_demo(PCB *proc, int owner_id) {
    printf("\n=== IPC: POSIX Shared Memory Demo ===\n");
    printf("Writer owner_id=%d, Region owner_id=%d\n\n",
           proc->owner_id, owner_id);

    /* ── Create shared memory region ── */
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (fd == -1) { perror("shm_open"); return; }

    if (ftruncate(fd, SHM_SIZE) != 0) {
        perror("ftruncate");
        close(fd);
        shm_unlink(SHM_NAME);
        return;
    }

    SharedMetrics *shm = mmap(NULL, SHM_SIZE,
                               PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) {
        perror("mmap");
        close(fd);
        shm_unlink(SHM_NAME);
        return;
    }
    close(fd);   /* fd no longer needed after mmap */

    /* Initialise mutex for cross-process synchronisation */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&shm->lock, &attr);
    pthread_mutexattr_destroy(&attr);

    /* Set owner */
    shm->owner_id = owner_id;

    /* ── Fork: parent writes, child reads ── */
    pid_t child = fork();
    if (child == -1) { perror("fork"); goto cleanup; }

    if (child == 0) {
        /* ─ Child: reader ─ */
        /* Re-attach to the same region */
        int cfd = shm_open(SHM_NAME, O_RDWR, 0600);
        if (cfd == -1) { perror("child shm_open"); exit(EXIT_FAILURE); }
        SharedMetrics *cshm = mmap(NULL, SHM_SIZE,
                                    PROT_READ | PROT_WRITE,
                                    MAP_SHARED, cfd, 0);
        close(cfd);
        if (cshm == MAP_FAILED) { perror("child mmap"); exit(EXIT_FAILURE); }

        /* Access-control check */
        if (!access_allowed(cshm, proc->owner_id)) {
            printf("[SHM-Child] ACCESS DENIED: owner_id %d cannot read "
                   "region owned by %d\n", proc->owner_id, cshm->owner_id);
            munmap(cshm, SHM_SIZE);
            exit(EXIT_FAILURE);
        }

        pthread_mutex_lock(&cshm->lock);
        printf("[SHM-Child]  Read: pid=%d burst=%d remaining=%d state=%d\n",
               cshm->pid, cshm->burst_time,
               cshm->remaining_time, cshm->state);
        pthread_mutex_unlock(&cshm->lock);

        munmap(cshm, SHM_SIZE);
        exit(EXIT_SUCCESS);
    }

    /* ─ Parent: writer ─ */
    pthread_mutex_lock(&shm->lock);
    shm->pid            = proc->pid;
    shm->burst_time     = proc->burst_time;
    shm->remaining_time = proc->remaining_time;
    shm->state          = proc->state;
    printf("[SHM-Parent] Wrote: pid=%d burst=%d remaining=%d state=%d\n",
           shm->pid, shm->burst_time, shm->remaining_time, shm->state);
    pthread_mutex_unlock(&shm->lock);

    /* Wait for child */
    int status;
    if (waitpid(child, &status, 0) == -1)
        perror("waitpid");

cleanup:
    pthread_mutex_destroy(&shm->lock);
    munmap(shm, SHM_SIZE);
    shm_unlink(SHM_NAME);    /* Remove the named region */

    printf("=== Shared Memory Demo Complete ===\n\n");
    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 2: Anonymous Pipe
 *
 * The parent serialises PCB data to a simple text format and writes
 * it into the write-end of an anonymous pipe.  A forked child reads
 * the read-end, parses each field, and prints the reconstructed PCB.
 *
 * This is exactly how shell pipelines work:
 *   echo "data" | grep "pattern"
 *   └─ parent writes to pipe   └─ child reads from pipe
 * ═══════════════════════════════════════════════════════════════════ */

/* Simple text serialisation: one field per line, key=value */
static void serialise_pcb(char *buf, size_t bufsz, const PCB *p) {
    snprintf(buf, bufsz,
             "pid=%d\n"
             "name=%s\n"
             "state=%d\n"
             "priority=%d\n"
             "burst_time=%d\n"
             "arrival_time=%d\n"
             "remaining_time=%d\n"
             "memory_req_kb=%d\n"
             "thread_count=%d\n"
             "owner_id=%d\n"
             "END\n",
             p->pid, p->name, p->state, p->priority,
             p->burst_time, p->arrival_time, p->remaining_time,
             p->memory_req_kb, p->thread_count, p->owner_id);
}

static void parse_pcb(const char *buf, PCB *out) {
    memset(out, 0, sizeof(*out));
    const char *line = buf;
    char key[32], val[128];

    while (*line) {
        int n = sscanf(line, "%31[^=]=%127[^\n]\n", key, val);
        if (n == 2) {
            if      (strcmp(key, "pid")           == 0) out->pid           = atoi(val);
            else if (strcmp(key, "name")          == 0) strncpy(out->name, val, 63);
            else if (strcmp(key, "state")         == 0) out->state         = atoi(val);
            else if (strcmp(key, "priority")      == 0) out->priority      = atoi(val);
            else if (strcmp(key, "burst_time")    == 0) out->burst_time    = atoi(val);
            else if (strcmp(key, "arrival_time")  == 0) out->arrival_time  = atoi(val);
            else if (strcmp(key, "remaining_time")== 0) out->remaining_time= atoi(val);
            else if (strcmp(key, "memory_req_kb") == 0) out->memory_req_kb = atoi(val);
            else if (strcmp(key, "thread_count")  == 0) out->thread_count  = atoi(val);
            else if (strcmp(key, "owner_id")      == 0) out->owner_id      = atoi(val);
            else if (strcmp(key, "END")           == 0) break;
        }
        /* Advance to next line */
        while (*line && *line != '\n') line++;
        if (*line == '\n') line++;
    }
}

void ipc_pipe_demo(PCB *procs, int count) {
    printf("\n=== IPC: Anonymous Pipe Demo ===\n");
    printf("Parent serialises %d PCB(s) → pipe → child parses & prints\n\n",
           count);

    int pipefd[2];
    if (pipe(pipefd) != 0) { perror("pipe"); return; }

    pid_t child = fork();
    if (child == -1) { perror("fork"); close(pipefd[0]); close(pipefd[1]); return; }

    if (child == 0) {
        /* ─ Child: reader ─ */
        close(pipefd[1]);  /* close write-end */

        char buf[1024];
        PCB parsed;

        /* Read each PCB message (terminated by "END\n") */
        for (int i = 0; i < count; i++) {
            memset(buf, 0, sizeof(buf));
            ssize_t total = 0;
            char tmp[8];
            ssize_t r;

            /* Read until we see "END\n" */
            while ((r = read(pipefd[0], tmp, 1)) > 0) {
                if (total + 1 < (ssize_t)sizeof(buf))
                    buf[total++] = tmp[0];
                /* Detect END marker */
                if (total >= 4 &&
                    strncmp(buf + total - 4, "END\n", 4) == 0)
                    break;
            }
            if (r == -1) { perror("child read"); break; }

            parse_pcb(buf, &parsed);
            printf("[Pipe-Child] Received PCB: pid=%d name='%s' "
                   "state=%d burst=%d\n",
                   parsed.pid, parsed.name,
                   parsed.state, parsed.burst_time);
            fflush(stdout);
        }

        close(pipefd[0]);
        exit(EXIT_SUCCESS);
    }

    /* ─ Parent: writer ─ */
    close(pipefd[0]);  /* close read-end */

    char buf[1024];
    for (int i = 0; i < count; i++) {
        serialise_pcb(buf, sizeof(buf), &procs[i]);
        ssize_t len = (ssize_t)strlen(buf);
        if (write(pipefd[1], buf, (size_t)len) != len)
            perror("parent write");
        else
            printf("[Pipe-Parent] Sent PCB: pid=%d name='%s'\n",
                   procs[i].pid, procs[i].name);
        fflush(stdout);
    }

    close(pipefd[1]);  /* EOF signals child to stop reading */

    int status;
    if (waitpid(child, &status, 0) == -1)
        perror("waitpid");

    printf("=== Pipe Demo Complete ===\n\n");
    fflush(stdout);
}
