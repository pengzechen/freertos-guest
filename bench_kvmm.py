#!/usr/bin/env python3
"""Run repeated FreeRTOS benchmark rounds under x-kernel kvmm."""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    import pexpect
except ImportError:
    sys.exit("pexpect is required: sudo apt-get install python3-pexpect")

import bench_qemu


ROOT = Path(__file__).resolve().parent
XKERNEL = ROOT.parent / "x-kernel"
DEFAULT_OUTDIR = ROOT / "bench-results-kvmm"
DEFAULT_MEM_BASE = "0x70000000"

PAT_SHELL = b"~#"
PAT_DONE = b"===== Done ====="
PAT_CRASHES = [
    b"unhandled exit, stopping VMM",
    b"Unhandled sync EC=",
    b"panic",
    b"PANIC",
]


def run_command(command: list[str], *, cwd: Path) -> subprocess.CompletedProcess[str]:
    print("$ " + " ".join(command), flush=True)
    return subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def build_freertos(mem_base: str) -> None:
    result = run_command(["make", "clean"], cwd=ROOT)
    if result.returncode != 0:
        raise RuntimeError(result.stdout)

    result = run_command(["make", f"MEM_BASE={mem_base}"], cwd=ROOT)
    if result.returncode != 0:
        raise RuntimeError(result.stdout)


def inject_freertos(disk_img: Path) -> None:
    if not disk_img.exists():
        raise RuntimeError(f"disk image not found: {disk_img}")
    src = ROOT / "freertos.bin"
    if not src.exists():
        raise RuntimeError(f"FreeRTOS binary not found: {src}")

    # Ignore rm failure: debugfs returns an error if the file does not exist.
    run_command(["debugfs", "-w", str(disk_img), "-R", "rm /freertos.bin"], cwd=ROOT)
    result = run_command(
        ["debugfs", "-w", str(disk_img), "-R", f"write {src} /freertos.bin"],
        cwd=ROOT,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stdout)


def build_xkernel() -> None:
    result = run_command(["cp", "platforms/aarch64-qemu-virt/defconfig", ".config"], cwd=XKERNEL)
    if result.returncode != 0:
        raise RuntimeError(result.stdout)
    result = run_command(["make", "defconfig"], cwd=XKERNEL)
    if result.returncode != 0:
        raise RuntimeError(result.stdout)
    result = run_command(["make", "build"], cwd=XKERNEL)
    if result.returncode != 0:
        raise RuntimeError(result.stdout)


def terminate_child(child: pexpect.spawn) -> None:
    try:
        child.sendcontrol("a")
        time.sleep(0.05)
        child.send(b"x")
        child.expect(pexpect.EOF, timeout=3)
    except Exception:
        pass
    try:
        child.close(force=True)
    except Exception:
        pass
    if child.isalive():
        try:
            child.kill(signal.SIGKILL)
        except Exception:
            pass


def run_round(round_index: int, args: argparse.Namespace, outdir: Path) -> list[dict[str, int | str]]:
    log_path = outdir / "logs" / f"round-{round_index:03d}.log"
    log_file = log_path.open("wb")
    make_cmd = f"make -C {XKERNEL} VSOCK=n QEMU_NICE={args.qemu_nice} justrun"
    boot_cmd = f'echo "boot /freertos.bin @{args.mem_base}" > /dev/kvmm'

    print(f"[run] round {round_index}/{args.rounds}", flush=True)
    child = pexpect.spawn(
        "/bin/bash",
        ["-c", make_cmd],
        cwd=str(XKERNEL),
        timeout=args.timeout,
        encoding=None,
        logfile=log_file,
    )

    try:
        idx = child.expect([PAT_SHELL, pexpect.TIMEOUT, pexpect.EOF], timeout=args.boot_timeout)
        if idx != 0:
            raise RuntimeError(f"round {round_index}: x-kernel shell did not appear; see {log_path}")

        time.sleep(args.shell_settle)
        child.sendline(boot_cmd.encode())

        patterns = [PAT_DONE, *PAT_CRASHES, pexpect.TIMEOUT, pexpect.EOF]
        idx = child.expect(patterns, timeout=args.timeout)
        if idx == 0:
            # Keep the VM alive briefly after Done so trailing output reaches the log.
            time.sleep(args.done_grace)
        elif 1 <= idx <= len(PAT_CRASHES):
            ctx = (child.before or b"")[-2000:] + (child.after or b"")
            raise RuntimeError(
                f"round {round_index}: kvmm crash before Done; see {log_path}\n"
                + ctx.decode("utf-8", errors="replace")
            )
        elif idx == len(PAT_CRASHES) + 1:
            raise RuntimeError(f"round {round_index}: timeout before Done; see {log_path}")
        else:
            raise RuntimeError(f"round {round_index}: QEMU exited before Done; see {log_path}")

    finally:
        terminate_child(child)
        log_file.close()

    text = log_path.read_text(encoding="utf-8", errors="replace")
    rows, expected_tick_ns = bench_qemu.parse_log(text, round_index)
    for row in rows:
        if row["metric"] == "Tick Delta" and expected_tick_ns is not None:
            row["expected"] = expected_tick_ns
            row["avg_error"] = int(row["avg"]) - expected_tick_ns
        else:
            row["expected"] = ""
            row["avg_error"] = ""
    return rows


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-n", "--rounds", type=int, default=3, help="number of kvmm benchmark rounds")
    parser.add_argument("-o", "--outdir", type=Path, default=None, help="output directory")
    parser.add_argument("--timeout", type=int, default=90, help="timeout after sending kvmm boot command")
    parser.add_argument("--boot-timeout", type=int, default=90, help="timeout waiting for x-kernel shell")
    parser.add_argument("--mem-base", default=DEFAULT_MEM_BASE, help="kvmm guest memory base and FreeRTOS MEM_BASE")
    parser.add_argument("--disk-img", type=Path, default=XKERNEL / "disk.img", help="x-kernel disk image")
    parser.add_argument("--qemu-nice", default="-12", help="QEMU_NICE value passed to make justrun")
    parser.add_argument("--shell-settle", type=float, default=0.3, help="delay after host shell prompt")
    parser.add_argument("--done-grace", type=float, default=0.5, help="delay after Done before killing QEMU")
    parser.add_argument("--skip-freertos-build", action="store_true", help="reuse existing freertos.bin")
    parser.add_argument("--skip-inject", action="store_true", help="do not update disk.img:/freertos.bin")
    parser.add_argument("--x-build", action="store_true", help="build x-kernel before running")
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

    if args.x_build:
        build_xkernel()
    if not args.skip_freertos_build:
        build_freertos(args.mem_base)
    if not args.skip_inject:
        inject_freertos(args.disk_img)

    all_rows: list[dict[str, int | str]] = []
    for round_index in range(1, args.rounds + 1):
        all_rows.extend(run_round(round_index, args, outdir))

    summaries = bench_qemu.write_results(all_rows, outdir)
    if not args.no_plot:
        bench_qemu.plot_results(all_rows, outdir)
    bench_qemu.print_summary(summaries)

    print(f"\nResults: {outdir}")
    print(f"CSV:     {outdir / 'results.csv'}")
    print(f"Summary: {outdir / 'summary.csv'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
