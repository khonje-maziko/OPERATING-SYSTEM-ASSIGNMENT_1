/*
 * thread_manager.c
 * ────────────────
 * Implements:
 *  1. A fixed-size POSIX thread pool with mutex + condvar task queue.
 *  2. Many-to-One threading model (cooperative, using setjmp/longjmp).
 *  3. One-to-One threading model (each thread → 1 pthread).
 *  4. Race condition demo (compile with -DRACE_DEMO to disable mutex).
 *  5. Producer-consumer using semaphores (sem_init).
 *  6. Deadlock demonstration + fix via consistent lock ordering.
 *
 * Threading model summary:
 *  Many-to-One: All user threads share ONE kernel thread (this pthread).
 *               If one blocks, ALL block — demonstrated via longjmp yield.
 *  One-to-One:  Each user thread maps to its own pthread (kernel thread).
 *               True parallelism on multi-core; shown via parallel sum.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <setjmp.h>
#include <unistd.h>
#include <errno.h>

#include "include/eduos.h"

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 1: Thread Pool
 * ═══════════════════════════════════════════════════════════════════ */

static pthread_t       pool_threads[THREAD_POOL_SIZE];
static Task            task_queue[TASK_QUEUE_SIZE];
static int             task_head  = 0;
static int             task_tail  = 0;
static int             task_count = 0;
static int             pool_running = 0;

static pthread_mutex_t pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  pool_cond  = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  done_cond  = PTHREAD_COND_INITIALIZER;
static int             active_tasks = 0;

/* Worker thread — blocks on condvar until a task arrives */
static void *worker_main(void *arg) {
    int id = *(int *)arg;
    free(arg);

    printf("[ThreadPool] Worker %d started\n", id);
    fflush(stdout);

    for (;;) {
        if (pthread_mutex_lock(&pool_mutex) != 0) {
            perror("worker: pthread_mutex_lock");
            break;
        }

        /* Wait for work or shutdown signal */
        while (task_count == 0 && pool_running)
            pthread_cond_wait(&pool_cond, &pool_mutex);

        if (!pool_running && task_count == 0) {
            pthread_mutex_unlock(&pool_mutex);
            break;  /* Graceful shutdown */
        }

        /* Dequeue one task */
        Task t = task_queue[task_head];
        task_head = (task_head + 1) % TASK_QUEUE_SIZE;
        task_count--;
        active_tasks++;

        pthread_mutex_unlock(&pool_mutex);

        /* Execute task outside the lock */
        t.func(t.arg);

        /* Signal completion */
        if (pthread_mutex_lock(&pool_mutex) != 0)
            perror("worker: pthread_mutex_lock (done)");
        active_tasks--;
        pthread_cond_signal(&done_cond);
        pthread_mutex_unlock(&pool_mutex);
    }

    printf("[ThreadPool] Worker %d exiting\n", id);
    fflush(stdout);
    return NULL;
}

void thread_pool_init(void) {
    pool_running = 1;
    task_head = task_tail = task_count = active_tasks = 0;

    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        int *id = malloc(sizeof(int));
        if (!id) { perror("malloc"); exit(EXIT_FAILURE); }
        *id = i;
        if (pthread_create(&pool_threads[i], NULL, worker_main, id) != 0) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
    }
    printf("[ThreadPool] Initialised with %d workers\n", THREAD_POOL_SIZE);
    fflush(stdout);
}

int thread_pool_submit(TaskFunc func, void *arg) {
    if (!func) return -1;

    if (pthread_mutex_lock(&pool_mutex) != 0) {
        perror("thread_pool_submit: lock");
        return -1;
    }

    if (task_count >= TASK_QUEUE_SIZE) {
        fprintf(stderr, "thread_pool_submit: queue full\n");
        pthread_mutex_unlock(&pool_mutex);
        return -1;
    }

    task_queue[task_tail].func = func;
    task_queue[task_tail].arg  = arg;
    task_tail  = (task_tail + 1) % TASK_QUEUE_SIZE;
    task_count++;

    pthread_cond_signal(&pool_cond);
    pthread_mutex_unlock(&pool_mutex);
    return 0;
}

/* Waits for all tasks to finish, then tears down worker threads */
void thread_pool_shutdown(void) {
    /* Wait until queue is drained */
    if (pthread_mutex_lock(&pool_mutex) != 0)
        perror("shutdown: lock");
    while (task_count > 0 || active_tasks > 0)
        pthread_cond_wait(&done_cond, &pool_mutex);
    pool_running = 0;
    pthread_cond_broadcast(&pool_cond);   /* wake all blocked workers */
    pthread_mutex_unlock(&pool_mutex);

    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_join(pool_threads[i], NULL) != 0)
            perror("pthread_join");
    }
    printf("[ThreadPool] All workers shut down cleanly\n");
    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 2: Many-to-One Model (cooperative via setjmp/longjmp)
 *
 * All "user threads" run on a SINGLE kernel thread (the caller).
 * Threads yield by calling m2o_yield(), which long-jumps back to the
 * scheduler.  If any thread blocks (e.g., on I/O), ALL threads block
 * because there is only one kernel thread underneath.
 * ═══════════════════════════════════════════════════════════════════ */

#define M2O_MAX_THREADS 4
#define M2O_STACK_SIZE  65536

typedef struct {
    jmp_buf  ctx;
    void    (*func)(int);
    int      id;
    int      finished;
    char    *stack;          /* heap-allocated stack (not used by setjmp,
                              * but allocated to show the concept)       */
} M2OThread;

static M2OThread  m2o_threads[M2O_MAX_THREADS];
static int        m2o_count    = 0;
static int        m2o_current  = 0;
static jmp_buf    m2o_scheduler_ctx;

/* Called by a user thread to voluntarily yield the CPU */
static void m2o_yield(void) {
    if (setjmp(m2o_threads[m2o_current].ctx) == 0)
        longjmp(m2o_scheduler_ctx, 1);  /* back to scheduler */
    /* Execution resumes here when this thread is rescheduled */
}

/* Simple round-robin scheduler for Many-to-One threads */
static void m2o_scheduler_run(void) {
    /* setjmp returns non-zero each time a thread yields */
    setjmp(m2o_scheduler_ctx);

    /* Find the next unfinished thread */
    for (int i = 0; i < m2o_count; i++) {
        m2o_current = (m2o_current + 1) % m2o_count;
        if (!m2o_threads[m2o_current].finished) {
            /* Resume it */
            longjmp(m2o_threads[m2o_current].ctx, 1);
        }
    }
    /* All threads finished */
}

static void m2o_thread_body(int id) {
    printf("[M2O] Thread %d running (all on ONE kernel thread)\n", id);
    m2o_yield();
    printf("[M2O] Thread %d resumed → doing work\n", id);
    m2o_yield();
    printf("[M2O] Thread %d finished\n", id);
    fflush(stdout);
    m2o_threads[id].finished = 1;
    longjmp(m2o_scheduler_ctx, 1);
}

/*
 * Limitation note:
 *   Because all M2O threads share one kernel thread, a blocking
 *   system call (e.g., read() on a slow device) suspends ALL threads.
 *   One-to-One avoids this by giving each thread its own kernel thread.
 */
void demo_many_to_one(void) {
    printf("\n=== Many-to-One Threading Demo ===\n");
    printf("All user threads run on ONE kernel thread (cooperative).\n");
    printf("Blocking one thread blocks ALL — the key M2O limitation.\n\n");

    m2o_count = 0;

    for (int i = 0; i < 3; i++) {
        m2o_threads[i].id       = i;
        m2o_threads[i].func     = m2o_thread_body;
        m2o_threads[i].finished = 0;
        m2o_threads[i].stack    = malloc(M2O_STACK_SIZE);
        if (!m2o_threads[i].stack) { perror("malloc M2O stack"); return; }

        /* Bootstrap: setjmp saves context; run body on first longjmp */
        if (setjmp(m2o_threads[i].ctx) == 0) {
            m2o_count++;
            continue;           /* First pass: just save context         */
        }
        /* Second pass (after longjmp): actually execute the thread body */
        m2o_thread_body(i);
    }

    m2o_scheduler_run();

    for (int i = 0; i < m2o_count; i++)
        free(m2o_threads[i].stack);

    printf("=== Many-to-One Demo Complete ===\n\n");
    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 3: One-to-One Model (each user thread → 1 pthread)
 *
 * Each EduOS thread maps directly to a POSIX pthread (kernel thread).
 * The OS can run them truly in parallel on multiple CPU cores.
 * Demonstrated via a parallel partial-sum computation.
 * ═══════════════════════════════════════════════════════════════════ */

#define O2O_THREADS  4
#define ARRAY_SIZE   1000000

static long long o2o_array[ARRAY_SIZE];
static long long o2o_partial[O2O_THREADS];

typedef struct { int start; int end; int tid; } SumArgs;

static void *o2o_sum_worker(void *arg) {
    SumArgs *a = (SumArgs *)arg;
    long long sum = 0;
    for (int i = a->start; i < a->end; i++)
        sum += o2o_array[i];
    o2o_partial[a->tid] = sum;
    printf("[O2O] Thread %d summed [%d, %d) = %lld\n",
           a->tid, a->start, a->end, sum);
    fflush(stdout);
    free(a);
    return NULL;
}

void demo_one_to_one(void) {
    printf("\n=== One-to-One Threading Demo ===\n");
    printf("Each EduOS thread → its own pthread (kernel thread).\n");
    printf("True concurrency: parallel sum over %d elements.\n\n", ARRAY_SIZE);

    /* Initialise array: values 1..ARRAY_SIZE */
    for (int i = 0; i < ARRAY_SIZE; i++)
        o2o_array[i] = (long long)(i + 1);

    pthread_t threads[O2O_THREADS];
    int chunk = ARRAY_SIZE / O2O_THREADS;

    for (int i = 0; i < O2O_THREADS; i++) {
        SumArgs *a = malloc(sizeof(SumArgs));
        if (!a) { perror("malloc SumArgs"); return; }
        a->start = i * chunk;
        a->end   = (i == O2O_THREADS - 1) ? ARRAY_SIZE : (i + 1) * chunk;
        a->tid   = i;
        if (pthread_create(&threads[i], NULL, o2o_sum_worker, a) != 0) {
            perror("pthread_create O2O");
            free(a);
        }
    }

    for (int i = 0; i < O2O_THREADS; i++)
        pthread_join(threads[i], NULL);

    long long total = 0;
    for (int i = 0; i < O2O_THREADS; i++)
        total += o2o_partial[i];

    /* Expected: N*(N+1)/2 where N = ARRAY_SIZE */
    long long expected = (long long)ARRAY_SIZE * (ARRAY_SIZE + 1) / 2;
    printf("[O2O] Total = %lld | Expected = %lld | %s\n",
           total, expected, total == expected ? "CORRECT" : "WRONG");
    printf("=== One-to-One Demo Complete ===\n\n");
    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 4: Race Condition Demo
 *
 * Compile with -DRACE_DEMO for make race (no mutex).
 * Compile WITHOUT -DRACE_DEMO for make fixed (with mutex).
 *
 * Both targets increment a shared counter 1,000,000 times across
 * 4 threads.  Without a mutex, the result is non-deterministic.
 * ═══════════════════════════════════════════════════════════════════ */

#define RACE_THREADS   4
#define RACE_INCREMENTS 250000

static long long shared_counter = 0;

#ifndef RACE_DEMO
/* make fixed: protect with mutex */
static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

static void *race_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < RACE_INCREMENTS; i++) {
#ifdef RACE_DEMO
        /* NO protection — data race! */
        shared_counter++;
#else
        /* Protected — always correct */
        pthread_mutex_lock(&counter_mutex);
        shared_counter++;
        pthread_mutex_unlock(&counter_mutex);
#endif
    }
    return NULL;
}

void demo_race_condition(void) {
    long long expected = (long long)RACE_THREADS * RACE_INCREMENTS;

#ifdef RACE_DEMO
    printf("\n=== Race Condition Demo (make race — NO mutex) ===\n");
    printf("Expected: %lld  | Result may differ each run!\n\n", expected);
#else
    printf("\n=== Race Condition Fixed (make fixed — WITH mutex) ===\n");
    printf("Expected: %lld  | Result must always match.\n\n", expected);
#endif

    pthread_t threads[RACE_THREADS];
    shared_counter = 0;

    for (int i = 0; i < RACE_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, race_worker, NULL) != 0)
            perror("pthread_create race");
    }
    for (int i = 0; i < RACE_THREADS; i++)
        pthread_join(threads[i], NULL);

    printf("shared_counter = %lld  (expected %lld) — %s\n",
           shared_counter, expected,
           shared_counter == expected ? "CORRECT" : "DATA RACE DETECTED");
    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 5: Producer-Consumer (semaphores)
 * ═══════════════════════════════════════════════════════════════════ */

#define BUFFER_SIZE 8
#define NUM_ITEMS   20

static int    pc_buffer[BUFFER_SIZE];
static int    pc_in  = 0;
static int    pc_out = 0;
static sem_t  sem_empty;    /* counts empty slots  */
static sem_t  sem_full;     /* counts filled slots */
static pthread_mutex_t pc_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *producer(void *arg) {
    (void)arg;
    for (int i = 0; i < NUM_ITEMS; i++) {
        sem_wait(&sem_empty);                       /* wait for a slot  */
        pthread_mutex_lock(&pc_mutex);
        pc_buffer[pc_in] = i;
        pc_in = (pc_in + 1) % BUFFER_SIZE;
        printf("[Producer] produced item %d\n", i);
        fflush(stdout);
        pthread_mutex_unlock(&pc_mutex);
        sem_post(&sem_full);                        /* signal new item  */
    }
    return NULL;
}

static void *consumer(void *arg) {
    (void)arg;
    for (int i = 0; i < NUM_ITEMS; i++) {
        sem_wait(&sem_full);                        /* wait for item    */
        pthread_mutex_lock(&pc_mutex);
        int item = pc_buffer[pc_out];
        pc_out = (pc_out + 1) % BUFFER_SIZE;
        printf("[Consumer] consumed item %d\n", item);
        fflush(stdout);
        pthread_mutex_unlock(&pc_mutex);
        sem_post(&sem_empty);                       /* free the slot    */
    }
    return NULL;
}

void demo_producer_consumer(void) {
    printf("\n=== Producer-Consumer Demo (semaphores) ===\n\n");

    if (sem_init(&sem_empty, 0, BUFFER_SIZE) != 0) { perror("sem_init empty"); return; }
    if (sem_init(&sem_full,  0, 0)           != 0) { perror("sem_init full");  return; }

    pthread_t prod_t, cons_t;
    if (pthread_create(&prod_t, NULL, producer, NULL) != 0) perror("pthread_create producer");
    if (pthread_create(&cons_t, NULL, consumer, NULL) != 0) perror("pthread_create consumer");

    pthread_join(prod_t, NULL);
    pthread_join(cons_t, NULL);

    sem_destroy(&sem_empty);
    sem_destroy(&sem_full);

    printf("\n=== Producer-Consumer Demo Complete ===\n\n");
    fflush(stdout);
}

/* ═══════════════════════════════════════════════════════════════════
 * SECTION 6: Deadlock Demonstration + Fix
 *
 * DEADLOCK scenario: Thread A locks mutex1 then mutex2.
 *                    Thread B locks mutex2 then mutex1.
 *                    → circular wait → deadlock.
 *
 * FIX: Consistent lock ordering — both threads always lock
 *      mutex1 BEFORE mutex2.  Breaks the circular wait condition.
 * ═══════════════════════════════════════════════════════════════════ */

static pthread_mutex_t dl_mutex1 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t dl_mutex2 = PTHREAD_MUTEX_INITIALIZER;

#ifdef DEADLOCK_DEMO
/* Deadlock version — opposite ordering */
static void *dl_thread_a(void *arg) {
    (void)arg;
    pthread_mutex_lock(&dl_mutex1);
    printf("[Deadlock] Thread A locked mutex1, trying mutex2...\n");
    fflush(stdout);
    sleep(1);   /* ensure B locks mutex2 first */
    pthread_mutex_lock(&dl_mutex2);  /* will block forever */
    printf("[Deadlock] Thread A locked both (unreachable)\n");
    pthread_mutex_unlock(&dl_mutex2);
    pthread_mutex_unlock(&dl_mutex1);
    return NULL;
}

static void *dl_thread_b(void *arg) {
    (void)arg;
    pthread_mutex_lock(&dl_mutex2);
    printf("[Deadlock] Thread B locked mutex2, trying mutex1...\n");
    fflush(stdout);
    pthread_mutex_lock(&dl_mutex1);  /* will block forever */
    printf("[Deadlock] Thread B locked both (unreachable)\n");
    pthread_mutex_unlock(&dl_mutex1);
    pthread_mutex_unlock(&dl_mutex2);
    return NULL;
}
#else
/* Fixed version — consistent ordering (mutex1 always before mutex2) */
static void *dl_thread_a(void *arg) {
    (void)arg;
    pthread_mutex_lock(&dl_mutex1);
    pthread_mutex_lock(&dl_mutex2);
    printf("[DeadlockFix] Thread A holds both mutexes (mutex1→mutex2 order)\n");
    fflush(stdout);
    pthread_mutex_unlock(&dl_mutex2);
    pthread_mutex_unlock(&dl_mutex1);
    return NULL;
}

static void *dl_thread_b(void *arg) {
    (void)arg;
    pthread_mutex_lock(&dl_mutex1);  /* same order as A */
    pthread_mutex_lock(&dl_mutex2);
    printf("[DeadlockFix] Thread B holds both mutexes (mutex1→mutex2 order)\n");
    fflush(stdout);
    pthread_mutex_unlock(&dl_mutex2);
    pthread_mutex_unlock(&dl_mutex1);
    return NULL;
}
#endif

void demo_deadlock(void) {
#ifdef DEADLOCK_DEMO
    printf("\n=== Deadlock Demo (WARNING: program will hang!) ===\n");
    printf("Thread A: lock mutex1 → lock mutex2\n");
    printf("Thread B: lock mutex2 → lock mutex1\n");
    printf("Circular wait → deadlock.\n\n");
#else
    printf("\n=== Deadlock Fixed (consistent lock ordering) ===\n");
    printf("Both threads: lock mutex1 → lock mutex2\n");
    printf("No circular wait → no deadlock.\n\n");
#endif

    pthread_t ta, tb;
    pthread_create(&ta, NULL, dl_thread_a, NULL);
    pthread_create(&tb, NULL, dl_thread_b, NULL);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    printf("=== Deadlock Demo Complete ===\n\n");
    fflush(stdout);
}
