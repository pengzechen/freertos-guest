#!/usr/bin/env python3
"""Run and summarize repeated FreeRTOS bare-QEMU benchmark rounds."""

from __future__ import annotations

import argparse
import csv
import json
import re
import shutil
import statistics
import subprocess
import sys
from datetime import datetime
from pathlib import Path


ROOT = Path(__file__).resolve().parent
DEFAULT_OUTDIR = ROOT / "bench-results"
QEMU_MEM_BASE = "0x3f880000"

METRIC_RE = re.compile(
    r"\[(?P<name>Task Switch|Preemption|IRQ Latency|Tick Delta|Sem Shuffle)\]\s+"
    r"n=(?P<n>\d+)\s+avg=(?P<avg>\d+)\s+min=(?P<min>\d+)\s+"
    r"max=(?P<max>\d+)\s+jitter=(?P<jitter>\d+)\s+ns"
)
EXPECTED_RE = re.compile(r"\[Tick Delta\]\s+expected=(?P<expected>\d+)\s+ns")

METRICS = [
    "Task Switch",
    "Preemption",
    "IRQ Latency",
    "Tick Delta",
    "Sem Shuffle",
]
FIELDS = ["avg", "min", "max", "jitter"]


def run_command(command: list[str], *, timeout: int | None = None) -> subprocess.CompletedProcess[str]:
    print("$ " + " ".join(command), flush=True)
    return subprocess.run(
        command,
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
        check=False,
    )


def ensure_dtb(dtb: Path, qemu: str) -> None:
    if dtb.exists():
        return

    result = run_command(
        [
            qemu,
            "-M",
            f"virt,dumpdtb={dtb}",
            "-cpu",
            "cortex-a53",
            "-nographic",
            "-smp",
            "1",
            "-m",
            "256M",
        ]
    )
    if not dtb.exists():
        raise RuntimeError(f"failed to generate {dtb}:\n{result.stdout}")


def build_binary() -> None:
    result = run_command(["make", "clean"])
    if result.returncode != 0:
        raise RuntimeError(result.stdout)

    result = run_command(["make", f"MEM_BASE={QEMU_MEM_BASE}"])
    if result.returncode != 0:
        raise RuntimeError(result.stdout)


    shutil.copyfile(ROOT / "freertos.bin", ROOT / "freertos.qemu.bin")


def parse_log(text: str, round_index: int) -> tuple[list[dict[str, int | str]], int | None]:
    rows: list[dict[str, int | str]] = []
    expected_tick_ns: int | None = None

    for line in text.splitlines():
        metric_match = METRIC_RE.search(line)
        if metric_match:
            row: dict[str, int | str] = {
                "round": round_index,
                "metric": metric_match.group("name"),
            }
            for field in ["n", *FIELDS]:
                row[field] = int(metric_match.group(field))
            rows.append(row)
            continue

        expected_match = EXPECTED_RE.search(line)
        if expected_match:
            expected_tick_ns = int(expected_match.group("expected"))

    missing = sorted(set(METRICS) - {str(row["metric"]) for row in rows})
    if missing:
        raise RuntimeError(f"round {round_index}: missing metrics: {', '.join(missing)}")

    if "===== Done =====" not in text:
        raise RuntimeError(f"round {round_index}: benchmark did not finish")

    return rows, expected_tick_ns


def run_round(round_index: int, args: argparse.Namespace, outdir: Path) -> list[dict[str, int | str]]:
    log_path = outdir / "logs" / f"round-{round_index:03d}.log"
    command = [
        args.qemu,
        "-M",
        "virt",
        "-cpu",
        "cortex-a53",
        "-nographic",
        "-smp",
        "1",
        "-m",
        "256M",
        "-kernel",
        str(ROOT / "freertos.qemu.bin"),
        "-dtb",
        str(ROOT / "freertos.dtb"),
    ]

    print(f"[run] round {round_index}/{args.rounds}", flush=True)
    result = subprocess.run(
        command,
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=args.timeout,
        check=False,
    )
    log_path.write_text(result.stdout, encoding="utf-8")

    if result.returncode != 0:
        raise RuntimeError(f"round {round_index}: qemu exited with {result.returncode}; see {log_path}")

    rows, expected_tick_ns = parse_log(result.stdout, round_index)
    for row in rows:
        if row["metric"] == "Tick Delta" and expected_tick_ns is not None:
            row["expected"] = expected_tick_ns
            row["avg_error"] = int(row["avg"]) - expected_tick_ns
        else:
            row["expected"] = ""
            row["avg_error"] = ""

    return rows


def write_results(rows: list[dict[str, int | str]], outdir: Path) -> list[dict[str, int | str | float]]:
    result_path = outdir / "results.csv"
    fieldnames = ["round", "metric", "n", "avg", "min", "max", "jitter", "expected", "avg_error"]
    with result_path.open("w", newline="", encoding="utf-8") as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    summaries: list[dict[str, int | str | float]] = []
    for metric in METRICS:
        metric_rows = [row for row in rows if row["metric"] == metric]
        for field in FIELDS:
            values = [int(row[field]) for row in metric_rows]
            summaries.append(
                {
                    "metric": metric,
                    "field": field,
                    "rounds": len(values),
                    "mean": statistics.fmean(values),
                    "median": statistics.median(values),
                    "min": min(values),
                    "max": max(values),
                    "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
                }
            )

    summary_path = outdir / "summary.csv"
    with summary_path.open("w", newline="", encoding="utf-8") as csvfile:
        writer = csv.DictWriter(
            csvfile,
            fieldnames=["metric", "field", "rounds", "mean", "median", "min", "max", "stdev"],
        )
        writer.writeheader()
        writer.writerows(summaries)

    (outdir / "summary.json").write_text(json.dumps(summaries, indent=2), encoding="utf-8")
    return summaries


def plot_results(rows: list[dict[str, int | str]], outdir: Path) -> None:
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("[plot] skipped: matplotlib is not installed", flush=True)
        return

    rounds = sorted({int(row["round"]) for row in rows})

    fig, axes = plt.subplots(2, 1, figsize=(12, 9), sharex=True)
    for metric in METRICS:
        metric_rows = sorted((row for row in rows if row["metric"] == metric), key=lambda row: int(row["round"]))
        axes[0].plot(rounds, [int(row["avg"]) for row in metric_rows], marker="o", label=metric)
        axes[1].plot(rounds, [int(row["jitter"]) for row in metric_rows], marker="o", label=metric)

    axes[0].set_title("FreeRTOS/QEMU Benchmark Average Latency")
    axes[0].set_ylabel("avg (ns)")
    axes[0].grid(True, alpha=0.3)
    axes[0].legend()

    axes[1].set_title("FreeRTOS/QEMU Benchmark Jitter")
    axes[1].set_xlabel("round")
    axes[1].set_ylabel("jitter (ns)")
    axes[1].grid(True, alpha=0.3)
    axes[1].legend()

    fig.tight_layout()
    fig.savefig(outdir / "benchmark-lines.png", dpi=160)
    plt.close(fig)

    fig, axes = plt.subplots(1, 2, figsize=(14, 6))
    labels = METRICS
    avg_values = [[int(row["avg"]) for row in rows if row["metric"] == metric] for metric in labels]
    jitter_values = [[int(row["jitter"]) for row in rows if row["metric"] == metric] for metric in labels]

    axes[0].boxplot(avg_values, labels=labels, showmeans=True)
    axes[0].set_title("Average Latency Distribution")
    axes[0].set_ylabel("avg (ns)")
    axes[0].tick_params(axis="x", rotation=25)
    axes[0].grid(True, axis="y", alpha=0.3)

    axes[1].boxplot(jitter_values, labels=labels, showmeans=True)
    axes[1].set_title("Jitter Distribution")
    axes[1].set_ylabel("jitter (ns)")
    axes[1].tick_params(axis="x", rotation=25)
    axes[1].grid(True, axis="y", alpha=0.3)

    fig.tight_layout()
    fig.savefig(outdir / "benchmark-boxplot.png", dpi=160)
    plt.close(fig)
    print(f"[plot] wrote {outdir / 'benchmark-lines.png'}", flush=True)
    print(f"[plot] wrote {outdir / 'benchmark-boxplot.png'}", flush=True)


def print_summary(summaries: list[dict[str, int | str | float]]) -> None:
    print("\nSummary (mean ns across rounds)")
    print("metric, avg, min, max, jitter")
    by_metric = {metric: {} for metric in METRICS}
    for item in summaries:
        by_metric[str(item["metric"])][str(item["field"])] = item

    for metric in METRICS:
        values = [by_metric[metric][field]["mean"] for field in FIELDS]
        print(f"{metric}, " + ", ".join(f"{value:.1f}" for value in values))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-n", "--rounds", type=int, default=3, help="number of QEMU benchmark rounds")
    parser.add_argument("-o", "--outdir", type=Path, default=None, help="output directory")
    parser.add_argument("--timeout", type=int, default=90, help="timeout per QEMU round in seconds")
    parser.add_argument("--qemu", default="qemu-system-aarch64", help="QEMU executable")
    parser.add_argument("--skip-build", action="store_true", help="reuse existing freertos.qemu.bin")
    parser.add_argument("--no-plot", action="store_true", help="skip matplotlib charts")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.rounds <= 0:
        print("rounds must be positive", file=sys.stderr)
        return 2

    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    outdir = args.outdir or DEFAULT_OUTDIR / timestamp
    (outdir / "logs").mkdir(parents=True, exist_ok=True)

    ensure_dtb(ROOT / "freertos.dtb", args.qemu)
    if not args.skip_build:
        build_binary()

    all_rows: list[dict[str, int | str]] = []
    for round_index in range(1, args.rounds + 1):
        all_rows.extend(run_round(round_index, args, outdir))

    summaries = write_results(all_rows, outdir)
    if not args.no_plot:
        plot_results(all_rows, outdir)
    print_summary(summaries)

    print(f"\nResults: {outdir}")
    print(f"CSV:     {outdir / 'results.csv'}")
    print(f"Summary: {outdir / 'summary.csv'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
