

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np
import os

# ─── Output directory ───
OUTPUT_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(file))),
    "docs", "screenshots"
)
os.makedirs(OUTPUT_DIR, exist_ok=True)

# ─── Colour palette for processes ───
COLORS = [
    "#FF6B6B", "#4ECDC4", "#45B7D1", "#96CEB4",
    "#FFEAA7", "#DDA0DD", "#98D8C8", "#F7DC6F",
    "#BB8FCE", "#85C1E9", "#F0B27A", "#82E0AA",
]

IDLE_COLOR = "#D5D8DC"


def get_color(pid, pid_list):
    """Return consistent color for a given PID."""
    idx = pid_list.index(pid) % len(COLORS)
    return COLORS[idx]


# ═══════════════════════════════════════════════════════════
# GANTT CHART — ONE ALGORITHM
# ═══════════════════════════════════════════════════════════

def plot_gantt(schedule, processes, algo_name, filename):
    """
    Draw a Gantt chart for one scheduling algorithm.
    Shows idle gaps in grey, each process in its own color.
    """
    if not schedule:
        return

    pid_list = [p["pid"] for p in processes]
    name_map = {p["pid"]: p["name"] for p in processes}

    fig, ax = plt.subplots(figsize=(14, 5))
    fig.patch.set_facecolor("#1E1E2E")
    ax.set_facecolor("#1E1E2E")

    max_time = schedule[-1][2]
    legend_patches = []
    seen_pids = set()

    prev_end = 0

    for (pid, start, end) in schedule:
        # Draw idle gap if any
        if start > prev_end:
            ax.barh(
                0, start - prev_end,
                left=prev_end, height=0.5,
                color=IDLE_COLOR, edgecolor="#888888",
                linewidth=0.5
            )
            ax.text(
                prev_end + (start - prev_end) / 2, 0,
                "IDLE", ha="center", va="center",
                fontsize=7, color="#333333", fontweight="bold"
            )

        # Draw process bar
        color = get_color(pid, pid_list)
        ax.barh(
            0, end - start,
            left=start, height=0.5,
            color=color, edgecolor="white",
            linewidth=0.8
        )

        # Label inside bar
        bar_width = end - start
        if bar_width > 0.5:
            ax.text(
                start + bar_width / 2, 0,
                f"P{pid}", ha="center", va="center",
                fontsize=8, color="white", fontweight="bold"
            )

        # Add to legend once per PID
        if pid not in seen_pids:
            seen_pids.add(pid)
            legend_patches.append(
                mpatches.Patch(
                    color=color,
                    label=f"P{pid}: {name_map.get(pid, '')}"
                )
            )

        prev_end = end

    # Add idle to legend
    legend_patches.append(
        mpatches.Patch(color=IDLE_COLOR, label="IDLE")
    )

    # Formatting
    ax.set_xlim(0, max_time + 1)
    ax.set_ylim(-0.5, 0.8)
    ax.set_xlabel("Time Units", color="white", fontsize=11)
    ax.set_title(
        f"Gantt Chart — {algo_name}",
        color="white", fontsize=14, fontweight="bold", pad=15
    )

    # Time axis ticks at every unit
    ax.set_xticks(range(0, max_time + 2))
    ax.tick_params(colors="white")
    ax.set_yticks([])

    for spine in ax.spines.values():
        spine.set_edgecolor("#444444")

    ax.legend(
        handles=legend_patches,
        loc="upper right",
        fontsize=8,
        facecolor="#2D2D3E",
        labelcolor="white",
        framealpha=0.8
    )

    ax.grid(axis="x", color="#444444", linestyle="--",
            linewidth=0.5, alpha=0.7)

    plt.tight_layout()
    filepath = os.path.join(OUTPUT_DIR, filename)
    plt.savefig(filepath, dpi=150, bbox_inches="tight",
                facecolor=fig.get_facecolor())
    plt.close()
    print(f"[Gantt] Saved: {filepath}")
[5/25/2026 10:04 AM] haji: # ═══════════════════════════════════════════════════════════
# COMPARISON BAR CHARTS
# ═══════════════════════════════════════════════════════════

def plot_comparison(all_results, filename="comparison_charts.png"):
    """
    Side by side bar charts comparing all 4 algorithms
    for Average WT, Average TAT, and CPU Utilisation.
    """
    algos   = list(all_results.keys())
    metrics = {
        "Avg Waiting Time":    "avg_waiting_time",
        "Avg Turnaround Time": "avg_turnaround_time",
        "CPU Utilisation (%)": "cpu_utilisation",
    }

    fig, axes = plt.subplots(1, 3, figsize=(16, 6))
    fig.patch.set_facecolor("#1E1E2E")
    fig.suptitle(
        "Algorithm Comparison — EduOS Scheduler",
        color="white", fontsize=14, fontweight="bold"
    )

    bar_colors = ["#FF6B6B", "#4ECDC4", "#45B7D1", "#96CEB4"]

    for ax, (metric_label, metric_key) in zip(axes, metrics.items()):
        ax.set_facecolor("#2D2D3E")
        values = [
            all_results[a]["aggregate"].get(metric_key, 0)
            for a in algos
        ]
        bars = ax.bar(algos, values, color=bar_colors,
                      edgecolor="white", linewidth=0.8)

        # Value labels on top of bars
        for bar, val in zip(bars, values):
            ax.text(
                bar.get_x() + bar.get_width() / 2,
                bar.get_height() + 0.1,
                f"{val:.2f}",
                ha="center", va="bottom",
                color="white", fontsize=9, fontweight="bold"
            )

        ax.set_title(metric_label, color="white",
                     fontsize=11, fontweight="bold")
        ax.set_ylabel(metric_label, color="white", fontsize=9)
        ax.tick_params(colors="white", labelsize=8)
        ax.set_ylim(0, max(values) * 1.3 if values else 1)

        for spine in ax.spines.values():
            spine.set_edgecolor("#444444")

        ax.tick_params(axis="x", rotation=15)

    plt.tight_layout()
    filepath = os.path.join(OUTPUT_DIR, filename)
    plt.savefig(filepath, dpi=150, bbox_inches="tight",
                facecolor=fig.get_facecolor())
    plt.close()
    print(f"[Comparison] Saved: {filepath}")


# ═══════════════════════════════════════════════════════════
# GENERATE ALL CHARTS
# ═══════════════════════════════════════════════════════════

def generate_all_charts(all_results, processes):
    """Generate Gantt charts for all algorithms + comparison."""
    filenames = {
        "FCFS":     "gantt_fcfs.png",
        "SJF":      "gantt_sjf.png",
        "Priority": "gantt_priority.png",
    }

    for algo_name, data in all_results.items():
        # Generate filename for RR dynamically
        if algo_name.startswith("RR"):
            fname = "gantt_rr.png"
        else:
            fname = filenames.get(algo_name, f"gantt_{algo_name}.png")

        plot_gantt(
            data["schedule"],
            processes,
            algo_name,
            fname
        )

    # Comparison chart
    plot_comparison(all_results, "comparison_charts.png")
    print("[Charts] All charts generated successfully!")


# ═══════════════════════════════════════════════════════════
# STANDALONE TEST
# ═══════════════════════════════════════════════════════════

if name == "main":
    # Quick test with sample data
    test_processes = [
        {"pid": 1, "name": "proc_1",
         "arrival_time": 0, "burst_time": 8, "priority": 2},
        {"pid": 2, "name": "proc_2",
         "arrival_time": 1, "burst_time": 4, "priority": 1},
        {"pid": 3, "name": "proc_3",
         "arrival_time": 2, "burst_time": 6, "priority": 3},
    ]
    test_schedule = [
        (1, 0, 8), (2, 8, 12), (3, 12, 18)
    ]
    plot_gantt(test_schedule, test_processes,
               "FCFS Test", "test_gantt.png")
    print("Test chart saved!")
