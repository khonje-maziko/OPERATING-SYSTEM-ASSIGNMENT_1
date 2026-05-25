import json
import os
import subprocess
import sys
import time
import datetime
import signal
import threading
from pathlib import Path
from typing import Optional

# ─── Paths (all relative — no hard-coded absolutes) ──────────────────
BASE_DIR        = Path(__file__).resolve().parent.parent
C_CORE_DIR      = BASE_DIR / "c_core"
C_BINARY        = C_CORE_DIR / "eduos"
PYTHON_DIR      = BASE_DIR / "python_scheduler"
SCHEDULER_SCRIPT= PYTHON_DIR / "scheduler_sim.py"
PCB_SNAPSHOT    = C_CORE_DIR / "pcb_snapshot.json"
PROCESS_INPUT   = C_CORE_DIR / "input_processes.json"
REPORT_DIR      = BASE_DIR / "docs"
REPORT_DIR.mkdir(parents=True, exist_ok=True)

# ─── State codes (must match eduos.h) ────────────────────────────────
STATE_TERMINATED = 4

# ─── Colour codes for terminal output ────────────────────────────────
GREEN  = "\033[92m"
YELLOW = "\033[93m"
CYAN   = "\033[96m"
RED    = "\033[91m"
RESET  = "\033[0m"
BOLD   = "\033[1m"

def log(msg: str, colour: str = RESET) -> None:
    ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
    print(f"{colour}[{ts}] {msg}{RESET}", flush=True)


# ════════════════════════════════════════════════════════════════════
# Step 1 — Build C binary
# ════════════════════════════════════════════════════════════════════

def build_c_binary() -> bool:
    """
    Run `make all` in c_core/.  Returns True on success.
    Mirrors how an OS loader prepares an executable before exec().
    """
    log("Step 1: Building C binary (make all)…", CYAN)
    result = subprocess.run(
        ["make", "all"],
        cwd=str(C_CORE_DIR),
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        log(f"Build FAILED:\n{result.stderr}", RED)
        return False
    log(f"Build OK → {C_BINARY}", GREEN)
    return True


# ════════════════════════════════════════════════════════════════════
# Step 2 — Generate process list
# ════════════════════════════════════════════════════════════════════

SAMPLE_PROCESSES = [
    {"pid": 10, "name": "webserver",  "arrival_time": 0,  "burst_time": 12,
     "priority": 1, "memory_req_kb": 512,  "owner_id": 1},
    {"pid": 11, "name": "database",   "arrival_time": 1,  "burst_time": 8,
     "priority": 0, "memory_req_kb": 1024, "owner_id": 1},
    {"pid": 12, "name": "logger",     "arrival_time": 2,  "burst_time": 5,
     "priority": 3, "memory_req_kb": 128,  "owner_id": 2},
    {"pid": 13, "name": "scheduler",  "arrival_time": 0,  "burst_time": 10,
     "priority": 2, "memory_req_kb": 256,  "owner_id": 1},
    {"pid": 14, "name": "monitor",    "arrival_time": 3,  "burst_time": 6,
     "priority": 2, "memory_req_kb": 200,  "owner_id": 2},
    {"pid": 15, "name": "backup_svc", "arrival_time": 4,  "burst_time": 15,
     "priority": 4, "memory_req_kb": 512,  "owner_id": 2},
]

def write_process_input() -> None:
    """Write process list JSON for the C binary to consume."""
    log("Step 2: Writing process input → input_processes.json", CYAN)
    with open(PROCESS_INPUT, "w") as f:
        json.dump(SAMPLE_PROCESSES, f, indent=2)
    log(f"  Wrote {len(SAMPLE_PROCESSES)} processes to {PROCESS_INPUT}", GREEN)


# ════════════════════════════════════════════════════════════════════
# Step 3 — Launch C binary; capture stdout in real time
# ════════════════════════════════════════════════════════════════════

def stream_output(proc: subprocess.Popen, lines: list) -> None:
    """
    Background thread: reads C binary stdout line by line and prints
    it immediately — 'real-time' capture as required by spec.

    This mirrors how a shell pipeline reads from a child process's
    stdout file descriptor without waiting for the child to exit.
    """
    for line in iter(proc.stdout.readline, ""):
        stripped = line.rstrip()
        if stripped:
            print(f"  {YELLOW}[C]{RESET} {stripped}", flush=True)
            lines.append(stripped)


def launch_c_binary() -> tuple:
    """
    Launch eduos via subprocess.Popen.
    Returns (Popen instance, collected stdout lines list).

    OS Concept — System Calls:
      subprocess.Popen internally calls fork(2) + execve(2).
      fork(2):   kernel duplicates the Python process's address space.
      execve(2): kernel replaces the child's image with the C binary.
      This is exactly what edu_fork + edu_exec simulate.
    """
    log("Step 3: Launching C binary via subprocess.Popen…", CYAN)

    if not C_BINARY.exists():
        log(f"Binary not found: {C_BINARY}", RED)
        sys.exit(1)

    proc = subprocess.Popen(
        [str(C_BINARY)],
        cwd=str(C_CORE_DIR),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,   # merge stderr → stdout
        text=True,
        bufsize=1,                  # line-buffered
    )
    log(f"  C binary PID (host OS): {proc.pid}", GREEN)

    stdout_lines: list = []
    reader = threading.Thread(
        target=stream_output,
        args=(proc, stdout_lines),
        daemon=True,
    )
    reader.start()
    return proc, stdout_lines, reader


# ════════════════════════════════════════════════════════════════════
# Step 4 — Signal C binary via stdin to load process file
# ════════════════════════════════════════════════════════════════════

def signal_c_binary(proc: subprocess.Popen) -> None:
    """
    Write the path of input_processes.json to the C binary's stdin.
    The C binary (main_sim.c) reads this on startup and logs the path.

    In the current EduOS design the C binary runs its own full demo
    and writes pcb_snapshot.json automatically.  The stdin signal is
    the handshake that would allow an interactive C binary to load
    a dynamic workload — demonstrating the IPC/pipe concept at the
    controller level.
    """
    log("Step 4: Signalling C binary via stdin (process file path)…", CYAN)
    try:
        proc.stdin.write(str(PROCESS_INPUT) + "\n")
        proc.stdin.flush()
        log("  Signal sent.", GREEN)
    except BrokenPipeError:
        # C binary may have already exited — that's fine
        log("  (C binary stdin closed — binary completed normally)", YELLOW)


# ════════════════════════════════════════════════════════════════════
# Step 5 — Monitor pcb_snapshot.json until all processes TERMINATED
# ════════════════════════════════════════════════════════════════════

def wait_for_termination(timeout: float = 60.0) -> Optional[list]:
    """
    Poll pcb_snapshot.json every 200 ms.
    Returns the final PCB list when all entries are TERMINATED,
    or None on timeout.

    OS Concept — wait(2):
      Real wait(2) suspends the calling process until the kernel
      delivers SIGCHLD.  EduOS uses file polling instead because
      the C binary writes state to a JSON file rather than using
      kernel signals.  A production design would use inotify(7)
      on Linux for event-driven notification.
    """
    log("Step 5: Monitoring pcb_snapshot.json for completion…", CYAN)
    deadline = time.time() + timeout

    while time.time() < deadline:
        if PCB_SNAPSHOT.exists():
            try:
                with open(PCB_SNAPSHOT) as f:
                    pcbs = json.load(f)
                if pcbs:
                    active = [p for p in pcbs
                              if p.get("state_code", 0) != STATE_TERMINATED]
                    total = len(pcbs)
                    done  = total - len(active)
                    log(f"  PCB status: {done}/{total} terminated", YELLOW)
                    if not active:
                        log("  All processes TERMINATED.", GREEN)
                        return pcbs
            except (json.JSONDecodeError, KeyError):
                pass   # file still being written

        time.sleep(0.2)

    log("Timeout waiting for C binary termination.", RED)
    return None


# ════════════════════════════════════════════════════════════════════
# Step 6 — Hand off to Python scheduler
# ════════════════════════════════════════════════════════════════════

def run_scheduler(quantum: int = 4) -> Optional[dict]:
    """
    Import scheduler_sim directly (same Python process) and run all
    four algorithms on the PCB snapshot produced by the C binary.

    Returns a dict of per-algorithm metrics for the JSON report.

    OS Concept — OS Structure (monolithic vs microkernel):
      In a MONOLITHIC kernel (Linux), scheduling is built into the
      kernel itself — no IPC needed to call the scheduler.
      In a MICROKERNEL (Mach, QNX), the scheduler runs as a user-space
      server; the kernel sends it messages via IPC.
      EduOS mirrors the microkernel model: the C 'kernel' (process
      manager) communicates with the Python 'scheduler server' via
      JSON files (a simplified message-passing IPC).
    """
    log("Step 6: Handing PCB snapshot to Python scheduler…", CYAN)

    if not PCB_SNAPSHOT.exists():
        log(f"  pcb_snapshot.json not found — using sample processes.", YELLOW)
        snapshot_path = str(PYTHON_DIR / "sample_processes.csv")
        src_flag = "--file"
    else:
        snapshot_path = str(PCB_SNAPSHOT)
        src_flag = "--pcb"

    # Add python_scheduler to path and import scheduler_sim
    sys.path.insert(0, str(PYTHON_DIR))
    import importlib
    import scheduler_sim as sched

    # Load processes
    if src_flag == "--pcb":
        procs = sched.load_json(snapshot_path)
    else:
        procs = sched.load_csv(snapshot_path)

    if not procs:
        log("  No schedulable processes found in snapshot.", RED)
        return None

    log(f"  Loaded {len(procs)} process(es) for scheduling.", GREEN)

    algorithms = {
        "FCFS":                   lambda p: sched.fcfs(p),
        "SJF":                    lambda p: sched.sjf(p),
        "Priority (with ageing)": lambda p: sched.priority_schedule(p),
        f"Round Robin (Q={quantum})": lambda p: sched.round_robin(p, quantum),
    }

    all_metrics = {}
    for label, fn in algorithms.items():
        schedule, result_procs = fn(procs)
        sched.print_results(result_procs, schedule, label)
        sched.draw_gantt(schedule, result_procs, label,
                         f"gantt_{label.split()[0].lower()}_ctrl.png")
        stats = sched.aggregate(result_procs, schedule, label)
        all_metrics[label] = stats
        log(f"  {label}: Avg WT={stats['avg_wt']}  "
            f"Avg TAT={stats['avg_tat']}  CPU={stats['cpu_util']}%", GREEN)

    if len(all_metrics) > 1:
        sched.draw_comparison(list(all_metrics.values()))
        sched.print_comparison_table(list(all_metrics.values()))

    return all_metrics


# ════════════════════════════════════════════════════════════════════
# Step 7 — Generate simulation_report.json
# ════════════════════════════════════════════════════════════════════

def write_report(c_stdout: list, metrics: dict) -> Path:
    """
    Write a timestamped simulation_report.json containing:
      • timestamp, run metadata
      • raw C binary output lines
      • per-algorithm scheduling metrics
      • OS concept mapping

    OS Concept — Protection & Security:
      The owner_id field in SharedMetrics enforces that a process
      can only read/write shared memory it owns — analogous to how
      the CPU's protection rings (Ring 0 = kernel, Ring 3 = user)
      prevent user processes from accessing kernel memory directly.
      EduOS demonstrates this in ipc_module.c: the ACCESS DENIED
      message is printed when owner_id mismatches.
    """
    report = {
        "title":     "EduOS Simulation Report",
        "module":    "351 CS 2104 — Operating Systems",
        "timestamp": datetime.datetime.now().isoformat(),
        "c_binary": {
            "binary":      str(C_BINARY),
            "stdout_lines": len(c_stdout),
            "stdout":      c_stdout,
        },
        "scheduling_results": metrics,
        "os_concept_mapping": {
            "system_calls": {
                "edu_fork":  "fork(2)   — kernel duplicates address space (COW)",
                "edu_exec":  "execve(2) — kernel replaces process image with new ELF",
                "edu_wait":  "wait(2)   — kernel suspends parent until SIGCHLD",
                "edu_exit":  "_exit(2)  — kernel reclaims resources; zombie until wait",
            },
            "os_structure": {
                "kernel_space": [
                    "process_manager.c (PCB table, fork/exec/wait/exit)",
                    "thread_manager.c  (thread pool, mutex/condvar)",
                    "ipc_module.c      (shared memory, pipes)",
                ],
                "user_space": [
                    "scheduler_sim.py  (scheduling algorithms)",
                    "main_controller.py (orchestration)",
                    "gantt.py          (visualisation)",
                ],
                "monolithic_vs_microkernel": (
                    "Monolithic (Linux): scheduler built into kernel — no IPC needed. "
                    "Microkernel (Mach/QNX): scheduler is a user-space server receiving "
                    "messages via IPC. EduOS mirrors the microkernel model: C 'kernel' "
                    "communicates with Python 'scheduler' via JSON message-passing."
                ),
            },
            "protection_and_security": {
                "mechanism": "owner_id field in SharedMetrics struct",
                "analogy":   "CPU protection rings (Ring 0=kernel, Ring 3=user)",
                "demo":      "ipc_module.c prints ACCESS DENIED on owner_id mismatch",
            },
            "virtual_machine_concept": (
                "Each PCB's isolated memory_req_kb field mirrors a Type-2 hypervisor's "
                "guest memory allocation.  Just as VMware/VirtualBox allocates a fixed "
                "RAM region per VM guest, EduOS allocates memory_req_kb per process. "
                "Processes cannot access each other's memory — enforced by the PCB "
                "table boundary checks, analogous to EPT (Extended Page Tables) in "
                "hardware-assisted virtualisation."
            ),
        },
    }

    report_path = REPORT_DIR / "simulation_report.json"
    with open(report_path, "w") as f:
        json.dump(report, f, indent=2)

    log(f"Step 7: Report written → {report_path}", GREEN)
    return report_path


# ════════════════════════════════════════════════════════════════════
# Main pipeline
# ════════════════════════════════════════════════════════════════════

def main() -> None:
    print(f"\n{BOLD}{'═'*58}")
    print("   EduOS Main Controller — End-to-End Pipeline")
    print(f"   Module: 351 CS 2104 — Operating Systems")
    print(f"{'═'*58}{RESET}\n")

    # ── Step 1: Build ──
    if not build_c_binary():
        sys.exit(1)

    # ── Step 2: Write process input ──
    write_process_input()

    # ── Step 3: Launch C binary ──
    c_proc, c_stdout_lines, reader_thread = launch_c_binary()

    # ── Step 4: Signal via stdin ──
    signal_c_binary(c_proc)

    # ── Wait for C binary to finish ──
    log("Waiting for C binary to complete…", CYAN)
    try:
        c_proc.wait(timeout=45)
    except subprocess.TimeoutExpired:
        log("C binary timeout — sending SIGTERM", RED)
        c_proc.send_signal(signal.SIGTERM)
        c_proc.wait()

    reader_thread.join(timeout=5)
    log(f"C binary exited (return code: {c_proc.returncode})", GREEN)

    # ── Step 5: Monitor PCB snapshot ──
    # The C binary writes pcb_snapshot.json during its run.
    # We check the final state now that it has exited.
    final_pcbs = wait_for_termination(timeout=5.0)

    if final_pcbs is None:
        log("Warning: could not confirm all PCBs terminated — "
            "proceeding with available snapshot.", YELLOW)

    # ── Step 6: Run Python scheduler ──
    metrics = run_scheduler(quantum=4)

    if metrics is None:
        log("Scheduler returned no metrics.", RED)
        sys.exit(1)

    # ── Step 7: Write JSON report ──
    report_path = write_report(c_stdout_lines, metrics)

    # ── Done ──
    print(f"\n{BOLD}{'═'*58}")
    print("   EduOS Pipeline Complete")
    print(f"   Report: {report_path}")
    print(f"{'═'*58}{RESET}\n")


if __name__ == "__main__":
    main()
