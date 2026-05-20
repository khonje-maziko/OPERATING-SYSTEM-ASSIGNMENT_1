#ifndef EDUOS_H
#define EDUOS_H

#include <sys/types.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>

/* ─── PCB State Constants ─────────────────────────────────────────── */
#define STATE_NEW        0
#define STATE_READY      1
#define STATE_RUNNING    2
#define STATE_WAITING    3
#define STATE_TERMINATED 4

/* ─── Limits ──────────────────────────────────────────────────────── */
#define MAX_PROCESSES    64
#define THREAD_POOL_SIZE  4
#define TASK_QUEUE_SIZE  32
#define SHM_NAME        "/eduos_shm"
#define SHM_SIZE         4096

/* ─── Process Control Block ───────────────────────────────────────── */
typedef struct {
    pid_t  pid;                /* unique process ID                   */
    char   name[64];           /* process name / program              */
    int    state;              /* NEW|READY|RUNNING|WAITING|TERMINATED */
    int    priority;           /* 0 = highest                         */
    int    burst_time;         /* total CPU time needed               */
    int    arrival_time;       /* clock tick of arrival               */
    int    remaining_time;     /* used by preemptive algorithms       */
    int    memory_req_kb;      /* memory footprint in KB              */
    int    thread_count;       /* threads spawned by process          */
    time_t creation_time;      /* wall-clock timestamp                */
    /* Extended fields */
    pid_t  parent_pid;         /* PID of parent (-1 if root)         */
    int    exit_code;          /* set by edu_exit                    */
    int    owner_id;           /* for IPC access-control check       */
    int    is_active;          /* 1 = slot in use, 0 = free          */
} PCB;

/* ─── Shared Memory Metrics Struct ───────────────────────────────── */
typedef struct {
    int    pid;
    int    burst_time;
    int    remaining_time;
    int    state;
    int    owner_id;
    pthread_mutex_t lock;
} SharedMetrics;

/* ─── Thread Pool Task ────────────────────────────────────────────── */
typedef void (*TaskFunc)(void *arg);

typedef struct {
    TaskFunc func;
    void    *arg;
} Task;

/* ─── Global PCB Table (defined in process_manager.c) ────────────── */
extern PCB      pcb_table[MAX_PROCESSES];
extern int      pcb_count;
extern pid_t    next_pid;
extern pthread_mutex_t pcb_mutex;

/* ─── process_manager.c prototypes ───────────────────────────────── */
void  init_process_manager(void);
pid_t edu_fork(PCB *parent);
void  edu_exec(pid_t pid, char *prog_name, int burst_time);
int   edu_wait(pid_t parent_pid);
void  edu_exit(pid_t pid, int exit_code);
void  edu_ps(void);
void  serialize_pcb_to_json(const char *filename);
PCB  *find_pcb(pid_t pid);
const char *state_name(int state);

/* ─── thread_manager.c prototypes ────────────────────────────────── */
void  thread_pool_init(void);
int   thread_pool_submit(TaskFunc func, void *arg);
void  thread_pool_shutdown(void);
void  demo_one_to_one(void);
void  demo_many_to_one(void);
void  demo_race_condition(void);      /* compiled with -DRACE_DEMO    */
void  demo_producer_consumer(void);
void  demo_deadlock(void);

/* ─── ipc_module.c prototypes ────────────────────────────────────── */
void  ipc_shared_memory_demo(PCB *proc, int owner_id);
void  ipc_pipe_demo(PCB *procs, int count);

#endif /* EDUOS_H */
