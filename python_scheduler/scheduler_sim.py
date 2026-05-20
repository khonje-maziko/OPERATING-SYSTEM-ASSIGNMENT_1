#!/usr/bin/env python3
"""
scheduler_sim.py  —  EduOS Python Scheduling Simulator
=======================================================
Module: 351 CS 2104 — Operating Systems

Implements four CPU scheduling algorithms:
  1. FCFS     — First Come, First Served (non-preemptive)
  2. SJF      — Shortest Job First (non-preemptive)
  3. Priority — Non-preemptive with ageing (every 3 ticks → +1 priority)
  4. RR       — Round Robin (preemptive, configurable quantum)

Input sources:
  --random N [--seed S]   generate N random processes
  --file PATH             read CSV or JSON
  --pcb PATH              read pcb_snapshot.json from C process manager

Output:
  • Per-process metrics table (AT, BT, CT, TAT, WT, RT)
  • Aggregate stats (avg WT, avg TAT, avg RT, CPU util%, throughput)
  • Gantt chart PNG per algorithm   → docs/screenshots/
  • Comparison bar charts PNG       → docs/screenshots/
  • Rich/tabulate comparison table to stdout

Thread mode:
  --mode thread           treat each 'process' as a thread; group by pid;
                          apply context-switch overhead between threads
                          of different processes.
"""

import argparse
import csv
import json
import os
import random
import sys
import copy
from dataclasses import dataclass, field
from typing import List, Tuple, Optional

import matplotlib
matplotlib.use("Agg")          # non-interactive backend (no display needed)
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from tabulate import tabulate

# ─── Output directory ────────────────────────────────────────────────
SCREENSHOTS_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "docs", "screenshots"
)
os.makedirs(SCREENSHOTS_DIR, exist_ok=True)

# ─── Colour palette for Gantt charts (one per process) ───────────────
COLOURS = [
    "#4e79a7", "#f28e2b", "#e15759", "#76b7b2",
    "#59a14f", "#edc948", "#b07aa1", "#ff9da7",
    "#9c755f", "#bab0ac",
]

# ════════════════════════════════════════════════════════════════════
# Process dataclass
# ════════════════════════════════════════════════════════════════════

@dataclass
class Process:
    pid:           int
    name:          str
    arrival_time:  int
    burst_time:    int
    priority:      int          # lower = higher urgency
    memory_req_kb: int  = 256
    owner_id:      int  = 0
    # Computed by scheduler
    completion_time: int = 0
    waiting_time:    int = 0
    turnaround_time: int = 0
    response_time:   int = -1   # -1 = not yet started
    # Internal use
    remaining_time:  int = field(init=False)
    aged_priority:   int = field(init=False)

    def __post_init__(self):
        self.remaining_time = self.burst_time
        self.aged_priority  = self.priority

    def reset(self):
        self.remaining_time  = self.burst_time
        self.aged_priority   = self.priority
        self.completion_time = 0
        self.waiting_time    = 0
        self.turnaround_time = 0
        self.response_time   = -1


# Type alias: schedule = list of (pid, start, end) tuples
Schedule = List[Tuple[int, int, int]]


# ════════════════════════════════════════════════════════════════════
# Helper: deep-copy a process list so algorithms don't corrupt state
# ════════════════════════════════════════════════════════════════════

def _clone(procs: List[Process]) -> List[Process]:
    return [copy.deepcopy(p) for p in procs]


def _compute_metrics(procs: List[Process], schedule: Schedule) -> None:
    """
    Fill completion_time, turnaround_time, waiting_time, response_time
    for each process from the schedule.
    """
    pid_map = {p.pid: p for p in procs}

    # response_time = first time the process gets the CPU
    for pid, start, _end in schedule:
        p = pid_map.get(pid)
        if p and p.response_time == -1:
            p.response_time = start - p.arrival_time

    # completion_time = last time slice end for each pid
    last_end: dict = {}
    for pid, _start, end in schedule:
        last_end[pid] = end

    for p in procs:
        p.completion_time = last_end.get(p.pid, p.arrival_time)
        p.turnaround_time = p.completion_time - p.arrival_time
        p.waiting_time    = p.turnaround_time - p.burst_time
        if p.response_time == -1:
            p.response_time = 0


# ════════════════════════════════════════════════════════════════════
# Algorithm 1 — FCFS
# ════════════════════════════════════════════════════════════════════

def fcfs(procs: List[Process]) -> Tuple[Schedule, List[Process]]:
    """
    First Come First Served — non-preemptive.
    Ties in arrival_time broken by lower PID first.
    """
    work = _clone(procs)
    work.sort(key=lambda p: (p.arrival_time, p.pid))
    schedule: Schedule = []
    clock = 0

    for p in work:
        if clock < p.arrival_time:
            clock = p.arrival_time   # CPU idle gap
        start = clock
        clock += p.burst_time
        p.completion_time = clock
        schedule.append((p.pid, start, clock))

    _compute_metrics(work, schedule)
    return schedule, work


# ════════════════════════════════════════════════════════════════════
# Algorithm 2 — SJF (non-preemptive)
# ════════════════════════════════════════════════════════════════════

def sjf(procs: List[Process]) -> Tuple[Schedule, List[Process]]:
    """
    Shortest Job First — non-preemptive.
    Among processes available at current clock, pick shortest burst.
    Ties in burst_time broken by arrival_time then PID (FCFS order).
    """
    work = _clone(procs)
    remaining = list(work)
    schedule: Schedule = []
    clock = 0

    while remaining:
        # Processes that have arrived
        available = [p for p in remaining if p.arrival_time <= clock]
        if not available:
            clock = min(p.arrival_time for p in remaining)
            continue

        # Pick shortest; tie → FCFS order (arrival_time, pid)
        chosen = min(available, key=lambda p: (p.burst_time, p.arrival_time, p.pid))
        remaining.remove(chosen)

        start = clock
        clock += chosen.burst_time
        chosen.completion_time = clock
        schedule.append((chosen.pid, start, clock))

    _compute_metrics(work, schedule)
    return schedule, work


# ════════════════════════════════════════════════════════════════════
# Algorithm 3 — Priority (non-preemptive) with ageing
# ════════════════════════════════════════════════════════════════════

def priority_schedule(procs: List[Process]) -> Tuple[Schedule, List[Process]]:
    """
    Priority Scheduling — non-preemptive, lower number = higher urgency.

    Ageing:
      Every 3 time units a process spends WAITING in the ready queue,
      its aged_priority is decremented by 1 (making it more urgent).
      This prevents starvation of low-priority processes.

      aged_priority starts at p.priority.
      Floor is 0 (cannot exceed highest priority).

    The algorithm picks the ready process with the lowest aged_priority.
    Ties broken by arrival_time then PID.
    """
    work = _clone(procs)
    remaining = list(work)
    schedule: Schedule = []
    clock = 0

    # Track when each process first entered the ready queue
    ready_since: dict = {}

    while remaining:
        available = [p for p in remaining if p.arrival_time <= clock]

        if not available:
            clock = min(p.arrival_time for p in remaining)
            # Update ready_since for newly arrived
            for p in remaining:
                if p.arrival_time <= clock and p.pid not in ready_since:
                    ready_since[p.pid] = p.arrival_time
            continue

        # Update ready_since for all newly available processes
        for p in available:
            if p.pid not in ready_since:
                ready_since[p.pid] = clock

        # Apply ageing: every 3 ticks waiting → +1 priority (lower number)
        for p in available:
            wait_so_far = clock - ready_since[p.pid]
            age_bonus   = wait_so_far // 3
            p.aged_priority = max(0, p.priority - age_bonus)

        # Pick highest priority (lowest aged_priority number)
        chosen = min(available, key=lambda p: (p.aged_priority, p.arrival_time, p.pid))
        remaining.remove(chosen)
        del ready_since[chosen.pid]

        start = clock
        clock += chosen.burst_time
        chosen.completion_time = clock
        schedule.append((chosen.pid, start, clock))

    _compute_metrics(work, schedule)
    return schedule, work


# ════════════════════════════════════════════════════════════════════
# Algorithm 4 — Round Robin (preemptive)
# ════════════════════════════════════════════════════════════════════

def round_robin(procs: List[Process], quantum: int = 4) -> Tuple[Schedule, List[Process]]:
    """
    Round Robin — preemptive, user-defined time quantum.

    Ready queue is FIFO.  When a process arrives while another is running
    it joins the tail of the ready queue.  The running process, if not
    finished, rejoins the tail after its quantum expires.

    Context-switch cost: 0 (pure algorithm; thread mode adds overhead).
    """
    work = _clone(procs)
    # Sort by arrival so we can enqueue in arrival order
    arrivals = sorted(work, key=lambda p: (p.arrival_time, p.pid))

    schedule: Schedule = []
    clock = 0
    ready_queue: list = []
    idx = 0            # pointer into arrivals list
    n = len(arrivals)

    # Enqueue all processes that arrive at time 0
    while idx < n and arrivals[idx].arrival_time <= clock:
        ready_queue.append(arrivals[idx])
        idx += 1

    while ready_queue or idx < n:
        if not ready_queue:
            # CPU idle — jump to next arrival
            clock = arrivals[idx].arrival_time
            while idx < n and arrivals[idx].arrival_time <= clock:
                ready_queue.append(arrivals[idx])
                idx += 1

        p = ready_queue.pop(0)

        run_time = min(quantum, p.remaining_time)
        start    = clock
        clock   += run_time
        p.remaining_time -= run_time
        schedule.append((p.pid, start, clock))

        # Enqueue any new arrivals during this slice
        while idx < n and arrivals[idx].arrival_time <= clock:
            ready_queue.append(arrivals[idx])
            idx += 1

        if p.remaining_time > 0:
            ready_queue.append(p)     # preempted — rejoin tail
        else:
            p.completion_time = clock

    _compute_metrics(work, schedule)
    return schedule, work


# ════════════════════════════════════════════════════════════════════
# Thread Mode wrapper
# ════════════════════════════════════════════════════════════════════

CONTEXT_SWITCH_COST = 1   # 1 time unit overhead when switching processes

def apply_thread_mode(schedule: Schedule, procs: List[Process]) -> Schedule:
    """
    In thread mode each 'process' is a thread belonging to a process group
    (identified by owner_id or pid prefix).  A context switch between
    threads of DIFFERENT process groups costs CONTEXT_SWITCH_COST units.
    Same-group thread switches are free (shared address space).
    """
    if not schedule:
        return schedule

    pid_to_group = {p.pid: p.owner_id for p in procs}
    adjusted: Schedule = []
    clock_offset = 0
    prev_group = None

    for pid, start, end in schedule:
        group = pid_to_group.get(pid, pid)
        overhead = CONTEXT_SWITCH_COST if (prev_group is not None and group != prev_group) else 0
        new_start = start + clock_offset + overhead
        new_end   = end   + clock_offset + overhead
        clock_offset += overhead
        adjusted.append((pid, new_start, new_end))
        prev_group = group

    return adjusted


# ════════════════════════════════════════════════════════════════════
# Metrics aggregation
# ════════════════════════════════════════════════════════════════════

def aggregate(procs: List[Process], schedule: Schedule, label: str) -> dict:
    total_time = max(end for _, _, end in schedule) if schedule else 1
    busy_time  = sum(end - start for _, start, end in schedule)
    n = len(procs)

    avg_wt  = sum(p.waiting_time    for p in procs) / n
    avg_tat = sum(p.turnaround_time for p in procs) / n
    avg_rt  = sum(p.response_time   for p in procs) / n
    cpu_util = busy_time / total_time * 100
    throughput = n / total_time

    return {
        "algorithm":  label,
        "avg_wt":     round(avg_wt,  2),
        "avg_tat":    round(avg_tat, 2),
        "avg_rt":     round(avg_rt,  2),
        "cpu_util":   round(cpu_util, 2),
        "throughput": round(throughput, 4),
    }


# ════════════════════════════════════════════════════════════════════
# Per-process results table
# ════════════════════════════════════════════════════════════════════

def print_results(procs: List[Process], schedule: Schedule,
                  label: str, thread_mode: bool = False) -> None:
    print(f"\n{'='*60}")
    print(f"  {label}{'  [THREAD MODE]' if thread_mode else ''}")
    print(f"{'='*60}")

    rows = []
    for p in sorted(procs, key=lambda x: x.pid):
        rows.append([
            p.pid, p.name, p.arrival_time, p.burst_time,
            p.completion_time, p.turnaround_time,
            p.waiting_time, p.response_time,
        ])

    headers = ["PID", "Name", "AT", "BT", "CT", "TAT", "WT", "RT"]
    print(tabulate(rows, headers=headers, tablefmt="rounded_outline"))

    stats = aggregate(procs, schedule, label)
    print(f"\n  Avg WT={stats['avg_wt']}  Avg TAT={stats['avg_tat']}"
          f"  Avg RT={stats['avg_rt']}"
          f"  CPU Util={stats['cpu_util']}%"
          f"  Throughput={stats['throughput']} proc/unit\n")


# ════════════════════════════════════════════════════════════════════
# Gantt Chart
# ════════════════════════════════════════════════════════════════════

def draw_gantt(schedule: Schedule, procs: List[Process],
               label: str, filename: str,
               thread_mode: bool = False) -> None:
    """
    Horizontal bar Gantt chart.
    • Each process gets a unique colour.
    • Idle CPU gaps are shown in grey.
    • Time-axis ticks at every unit.
    • In thread mode, process groups are labelled on the Y-axis.
    """
    pid_colour = {}
    pid_name   = {p.pid: p.name for p in procs}
    colour_idx = 0
    for p in procs:
        pid_colour[p.pid] = COLOURS[colour_idx % len(COLOURS)]
        colour_idx += 1

    fig, ax = plt.subplots(figsize=(max(12, len(schedule) * 0.6), 4))
    ax.set_title(f"Gantt Chart — {label}"
                 + ("  [Thread Mode]" if thread_mode else ""),
                 fontsize=13, fontweight="bold")

    bar_height = 0.6
    y = 0.5

    # Detect and draw idle gaps
    prev_end = 0
    for pid, start, end in schedule:
        if start > prev_end:
            ax.broken_barh([(prev_end, start - prev_end)],
                           (y - bar_height/2, bar_height),
                           facecolors="#d3d3d3", edgecolor="white")
            ax.text(prev_end + (start - prev_end)/2, y, "IDLE",
                    ha="center", va="center", fontsize=7, color="#555")
        ax.broken_barh([(start, end - start)],
                       (y - bar_height/2, bar_height),
                       facecolors=pid_colour[pid], edgecolor="white", linewidth=0.5)
        ax.text(start + (end - start)/2, y,
                f"P{pid}", ha="center", va="center",
                fontsize=8, color="white", fontweight="bold")
        prev_end = max(prev_end, end)

    total_time = max(end for _, _, end in schedule)
    ax.set_xlim(0, total_time + 0.5)
    ax.set_ylim(0, 1)
    ax.set_xlabel("Time Units", fontsize=10)
    ax.set_xticks(range(0, total_time + 1))
    ax.tick_params(axis="x", labelsize=7)
    ax.set_yticks([])

    # Legend
    patches = [mpatches.Patch(color=pid_colour[p.pid],
                               label=f"P{p.pid}: {p.name}")
               for p in procs]
    patches.append(mpatches.Patch(color="#d3d3d3", label="IDLE"))
    ax.legend(handles=patches, loc="upper right",
              fontsize=7, ncol=min(4, len(patches)))

    plt.tight_layout()
    path = os.path.join(SCREENSHOTS_DIR, filename)
    plt.savefig(path, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"  [Saved] {path}")


# ════════════════════════════════════════════════════════════════════
# Comparison Charts
# ════════════════════════════════════════════════════════════════════

def draw_comparison(all_stats: List[dict]) -> None:
    """
    Side-by-side bar charts comparing all four algorithms for:
      Average WT, Average TAT, CPU Utilisation.
    """
    labels     = [s["algorithm"] for s in all_stats]
    avg_wt     = [s["avg_wt"]    for s in all_stats]
    avg_tat    = [s["avg_tat"]   for s in all_stats]
    cpu_util   = [s["cpu_util"]  for s in all_stats]

    x = range(len(labels))
    width = 0.25

    fig, axes = plt.subplots(1, 3, figsize=(15, 5))
    fig.suptitle("Algorithm Comparison", fontsize=14, fontweight="bold")

    metrics = [
        (axes[0], avg_wt,   "Average Waiting Time",     "#4e79a7"),
        (axes[1], avg_tat,  "Average Turnaround Time",  "#f28e2b"),
        (axes[2], cpu_util, "CPU Utilisation (%)",       "#59a14f"),
    ]

    for ax, vals, title, colour in metrics:
        bars = ax.bar(x, vals, color=colour, edgecolor="white", linewidth=0.8)
        ax.set_title(title, fontsize=11)
        ax.set_xticks(list(x))
        ax.set_xticklabels(labels, fontsize=9)
        ax.set_ylabel(title, fontsize=9)
        for bar, val in zip(bars, vals):
            ax.text(bar.get_x() + bar.get_width()/2,
                    bar.get_height() + 0.2,
                    str(val), ha="center", va="bottom", fontsize=9)

    plt.tight_layout()
    path = os.path.join(SCREENSHOTS_DIR, "comparison_charts.png")
    plt.savefig(path, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"  [Saved] {path}")


def print_comparison_table(all_stats: List[dict]) -> None:
    rows = [[s["algorithm"], s["avg_wt"], s["avg_tat"],
             s["avg_rt"], f"{s['cpu_util']}%", s["throughput"]]
            for s in all_stats]
    headers = ["Algorithm", "Avg WT", "Avg TAT", "Avg RT", "CPU Util", "Throughput"]
    print("\n" + "="*60)
    print("  COMPARISON TABLE — All Algorithms")
    print("="*60)
    print(tabulate(rows, headers=headers, tablefmt="rounded_outline"))
    print()


# ════════════════════════════════════════════════════════════════════
# Input Loaders
# ════════════════════════════════════════════════════════════════════

def load_csv(path: str) -> List[Process]:
    """
    CSV schema (required columns):
      pid, name, arrival_time, burst_time, priority
    Optional:
      memory_req_kb, owner_id
    """
    procs = []
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            procs.append(Process(
                pid           = int(row["pid"]),
                name          = row["name"],
                arrival_time  = int(row["arrival_time"]),
                burst_time    = int(row["burst_time"]),
                priority      = int(row["priority"]),
                memory_req_kb = int(row.get("memory_req_kb", 256)),
                owner_id      = int(row.get("owner_id", 0)),
            ))
    return procs


def load_json(path: str) -> List[Process]:
    """
    JSON schema: array of objects with same keys as CSV.
    Also accepts pcb_snapshot.json from the C process manager.
    """
    with open(path) as f:
        data = json.load(f)

    procs = []
    for item in data:
        # Skip terminated processes (state_code 4) from PCB snapshots
        if item.get("state_code", 0) == 4:
            continue
        procs.append(Process(
            pid           = int(item["pid"]),
            name          = item.get("name", f"proc_{item['pid']}"),
            arrival_time  = int(item.get("arrival_time", 0)),
            burst_time    = int(item.get("burst_time", 1)),
            priority      = int(item.get("priority", 0)),
            memory_req_kb = int(item.get("memory_req_kb", 256)),
            owner_id      = int(item.get("owner_id", 0)),
        ))
    return procs


def generate_random(n: int, seed: Optional[int] = None) -> List[Process]:
    """
    Generate N processes with reproducible random values.
    arrival_time: 0–10, burst_time: 1–15, priority: 0–5
    """
    if seed is not None:
        random.seed(seed)

    procs = []
    for i in range(1, n + 1):
        procs.append(Process(
            pid          = i,
            name         = f"proc_{i}",
            arrival_time = random.randint(0, 10),
            burst_time   = random.randint(1, 15),
            priority     = random.randint(0, 5),
        ))
    return procs


# ════════════════════════════════════════════════════════════════════
# Main
# ════════════════════════════════════════════════════════════════════

def parse_args():
    parser = argparse.ArgumentParser(
        description="EduOS CPU Scheduling Simulator — 351 CS 2104"
    )
    src = parser.add_mutually_exclusive_group(required=True)
    src.add_argument("--random", metavar="N", type=int,
                     help="Generate N random processes")
    src.add_argument("--file",   metavar="PATH",
                     help="Load from CSV or JSON file")
    src.add_argument("--pcb",    metavar="PATH",
                     help="Load from pcb_snapshot.json (C bridge)")

    parser.add_argument("--seed",    type=int, default=None,
                        help="Random seed for reproducibility")
    parser.add_argument("--quantum", type=int, default=4,
                        help="Round Robin time quantum (default: 4)")
    parser.add_argument("--mode",    choices=["process", "thread"],
                        default="process",
                        help="Scheduling mode (default: process)")
    parser.add_argument("--algo",
                        choices=["fcfs", "sjf", "priority", "rr", "all"],
                        default="all",
                        help="Algorithm to run (default: all)")
    return parser.parse_args()


def run_all(procs: List[Process], quantum: int,
            thread_mode: bool, algo_filter: str) -> None:

    algorithms = {
        "fcfs":     ("FCFS",                   lambda p: fcfs(p)),
        "sjf":      ("SJF",                    lambda p: sjf(p)),
        "priority": ("Priority (with ageing)", lambda p: priority_schedule(p)),
        "rr":       (f"Round Robin (Q={quantum})",
                                               lambda p: round_robin(p, quantum)),
    }

    if algo_filter != "all":
        algorithms = {k: v for k, v in algorithms.items() if k == algo_filter}

    all_stats = []

    for key, (label, fn) in algorithms.items():
        sched, result_procs = fn(procs)

        if thread_mode:
            sched = apply_thread_mode(sched, result_procs)
            _compute_metrics(result_procs, sched)

        print_results(result_procs, sched, label, thread_mode)
        draw_gantt(sched, result_procs, label,
                   f"gantt_{key}.png", thread_mode)
        all_stats.append(aggregate(result_procs, sched, label))

    if len(all_stats) > 1:
        print_comparison_table(all_stats)
        draw_comparison(all_stats)

        # Identify best algorithm per metric
        best_wt  = min(all_stats, key=lambda s: s["avg_wt"])
        best_tat = min(all_stats, key=lambda s: s["avg_tat"])
        best_cpu = max(all_stats, key=lambda s: s["cpu_util"])
        print("  Analysis:")
        print(f"    Lowest Avg WT  → {best_wt['algorithm']}  ({best_wt['avg_wt']})")
        print(f"    Lowest Avg TAT → {best_tat['algorithm']} ({best_tat['avg_tat']})")
        print(f"    Highest CPU%   → {best_cpu['algorithm']} ({best_cpu['cpu_util']}%)")
        print()


def main():
    args = parse_args()

    # ── Load processes ──
    if args.random:
        procs = generate_random(args.random, args.seed)
        print(f"Generated {args.random} random processes"
              + (f" (seed={args.seed})" if args.seed else "") + ".")
    elif args.file:
        ext = os.path.splitext(args.file)[1].lower()
        if ext == ".csv":
            procs = load_csv(args.file)
        else:
            procs = load_json(args.file)
        print(f"Loaded {len(procs)} processes from {args.file}.")
    else:  # --pcb
        procs = load_json(args.pcb)
        print(f"Loaded {len(procs)} processes from PCB snapshot {args.pcb}.")

    if not procs:
        print("Error: no processes loaded.", file=sys.stderr)
        sys.exit(1)

    thread_mode = (args.mode == "thread")

    if thread_mode:
        print(f"\n[Thread Mode] Context-switch cost = {CONTEXT_SWITCH_COST} unit "
              f"when switching process groups.\n")

    run_all(procs, args.quantum, thread_mode, args.algo)


if __name__ == "__main__":
    main()
