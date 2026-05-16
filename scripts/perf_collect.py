#!/usr/bin/env python3
"""
scripts/perf_collect.py — ChimeraOS Performance Metrics Collector
==================================================================

Boots the ChimeraOS integration kernel with scenario 'M', captures the
[METRICS] output from the serial port, measures net_rtt_ms host-side, and
appends a timestamped row to metrics/perf_history.csv.

After each run it regenerates metrics/perf_chart.png — a six-panel time-
series chart that makes regressions immediately visible.

Usage
-----
    python3 scripts/perf_collect.py [OPTIONS]

    --iso PATH          Path to chimaera_integration_test.iso
                        (default: <repo>/chimaera_integration_test.iso)
    --csv PATH          Path to the CSV history file
                        (default: <repo>/metrics/perf_history.csv)
    --chart PATH        Path to the output PNG chart
                        (default: <repo>/metrics/perf_chart.png)
    --searxng-host HOST SearXNG hostname/IP for net_rtt_ms measurement
                        (default: localhost)
    --searxng-port PORT SearXNG port (default: 8080)
    --runs N            Number of back-to-back runs to perform (default: 1)
    --timeout SECS      QEMU timeout per run in seconds (default: 120)
    --label LABEL       Optional label for this run (e.g. git SHA or tag)
    --no-chart          Skip chart generation (useful in headless CI)

Output CSV columns
------------------
    timestamp           ISO-8601 UTC timestamp
    label               Optional run label (git SHA, tag, etc.)
    boot_ms             Multiboot entry → first [METRICS] READY line (ms)
    mem_after_boot_bytes  Heap bytes used after boot initialisation
    ttfb_ms             Time from inference start to first token (ms)
    tokens_per_sec      100-token generation throughput (tokens/s)
    mem_after_infer_bytes Heap bytes used after inference completes
    net_rtt_ms          Round-trip time to SearXNG host (ms); -1 if unreachable

Regression detection
--------------------
After appending the new row, perf_collect.py compares each metric against
the rolling mean of the previous 10 runs.  If a metric deviates by more
than 20% it prints a WARNING line to stdout (and to the CSV comment header).
This makes the script suitable for use in CI pipelines:

    python3 scripts/perf_collect.py --runs 3 || exit 1

The script exits 0 even when regressions are detected (regressions are
warnings, not errors) unless --fail-on-regression is passed.

Design notes
------------
net_rtt_ms is measured host-side because the kernel has no network stack.
The measurement uses a raw TCP connect to the SearXNG host:port (not ICMP,
which requires root).  If the host is unreachable, net_rtt_ms is recorded
as -1.

The kernel emits net_rtt_ms=0 as a sentinel; perf_collect.py overwrites it
with the measured value before writing to CSV.
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional

# ── Paths ─────────────────────────────────────────────────────────────────────

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT  = SCRIPT_DIR.parent
DEFAULT_ISO   = REPO_ROOT / "chimaera_integration_test.iso"
DEFAULT_CSV   = REPO_ROOT / "metrics" / "perf_history.csv"
DEFAULT_CHART = REPO_ROOT / "metrics" / "perf_chart.png"

CSV_FIELDS = [
    "timestamp", "label",
    "boot_ms", "mem_after_boot_bytes", "ttfb_ms",
    "tokens_per_sec", "mem_after_infer_bytes", "net_rtt_ms",
]

# Metrics that should be lower = better (for regression direction)
LOWER_IS_BETTER = {"boot_ms", "ttfb_ms", "net_rtt_ms"}

# Regression threshold: warn if metric deviates by more than this fraction
REGRESSION_THRESHOLD = 0.20   # 20%

# Rolling window for regression baseline
REGRESSION_WINDOW = 10


# ── QEMU helpers ──────────────────────────────────────────────────────────────

def _mtools_env() -> dict:
    env = os.environ.copy()
    env["MTOOLSRC"] = "/dev/null"
    return env


def _build_disk(disk_path: str) -> None:
    """Create a 64 MiB FAT32 disk image with SCENARIO=M."""
    # Create sparse file
    with open(disk_path, "wb") as f:
        f.seek(64 * 1024 * 1024 - 1)
        f.write(b"\x00")
    subprocess.run(
        ["mkfs.fat", "-F", "32", "-n", "CHIMAERA", disk_path],
        check=True, capture_output=True, stdin=subprocess.DEVNULL,
    )
    # Write SCENARIO file
    with tempfile.NamedTemporaryFile(delete=False, suffix=".scn") as tmp:
        tmp.write(b"M")
        tmp_name = tmp.name
    try:
        subprocess.run(
            ["mcopy", "-i", disk_path, tmp_name, "::SCENARIO"],
            check=True, capture_output=True,
            env=_mtools_env(), stdin=subprocess.DEVNULL,
        )
    finally:
        os.unlink(tmp_name)


def _run_qemu(iso_path: str, disk_path: str, timeout: int) -> List[str]:
    """Boot QEMU and return the serial log lines."""
    with tempfile.NamedTemporaryFile(
        suffix=".log", delete=False, mode="w"
    ) as f:
        serial_log_path = f.name

    cmd = [
        "qemu-system-i386",
        "-m", "128",
        "-no-reboot",
        "-display", "none",
        "-serial", f"file:{serial_log_path}",
        "-boot", "d",
        "-cdrom", iso_path,
        "-drive", f"file={disk_path},if=ide,index=1,format=raw,cache=none",
    ]

    proc = subprocess.Popen(
        cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    deadline = time.time() + timeout
    done = False
    while time.time() < deadline:
        time.sleep(1)
        if os.path.exists(serial_log_path):
            with open(serial_log_path, "r", errors="replace") as f:
                content = f.read()
            if "[INTTEST] DONE" in content:
                done = True
                break

    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()

    if not done:
        print("  WARNING: QEMU did not reach [INTTEST] DONE within timeout",
              file=sys.stderr)

    lines: List[str] = []
    if os.path.exists(serial_log_path):
        with open(serial_log_path, "r", errors="replace") as f:
            lines = f.read().splitlines()
        os.unlink(serial_log_path)

    return lines


def _parse_metrics(lines: List[str]) -> Dict[str, int]:
    """Parse [METRICS] key=value lines from serial log."""
    metrics: Dict[str, int] = {}
    for line in lines:
        line = line.strip()
        if line.startswith("[METRICS] ") and "=" in line:
            kv = line[len("[METRICS] "):]
            k, _, v = kv.partition("=")
            try:
                metrics[k.strip()] = int(v.strip())
            except ValueError:
                pass
    return metrics


# ── Network RTT measurement ───────────────────────────────────────────────────

def _measure_rtt(host: str, port: int, attempts: int = 3) -> int:
    """
    Measure TCP connect RTT to host:port.
    Returns the median RTT in milliseconds, or -1 if unreachable.
    Uses TCP connect (not ICMP) so no root privileges are required.
    """
    rtts: List[float] = []
    for _ in range(attempts):
        try:
            t0 = time.perf_counter()
            s = socket.create_connection((host, port), timeout=5)
            t1 = time.perf_counter()
            s.close()
            rtts.append((t1 - t0) * 1000)
        except (OSError, socket.timeout):
            pass
        time.sleep(0.1)

    if not rtts:
        return -1
    rtts.sort()
    return int(rtts[len(rtts) // 2])


# ── CSV helpers ───────────────────────────────────────────────────────────────

def _load_history(csv_path: Path) -> List[Dict[str, str]]:
    """Load existing CSV rows (returns empty list if file does not exist)."""
    if not csv_path.exists():
        return []
    with open(csv_path, "r", newline="") as f:
        reader = csv.DictReader(f)
        return list(reader)


def _append_row(csv_path: Path, row: Dict[str, str]) -> None:
    """Append a row to the CSV, creating it with headers if necessary."""
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    write_header = not csv_path.exists()
    with open(csv_path, "a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        if write_header:
            writer.writeheader()
        writer.writerow(row)


# ── Regression detection ──────────────────────────────────────────────────────

def _check_regressions(
    history: List[Dict[str, str]],
    current: Dict[str, str],
    threshold: float = REGRESSION_THRESHOLD,
    window: int = REGRESSION_WINDOW,
) -> List[str]:
    """
    Compare current metric values against the rolling mean of the last
    `window` historical rows.  Return a list of warning strings.
    """
    warnings: List[str] = []
    numeric_fields = [f for f in CSV_FIELDS
                      if f not in ("timestamp", "label")]

    # Use only the most recent `window` rows
    recent = history[-window:] if len(history) >= window else history

    if not recent:
        return []

    for field in numeric_fields:
        try:
            current_val = float(current[field])
        except (KeyError, ValueError):
            continue

        # Skip sentinel values
        if current_val < 0:
            continue

        historical_vals = []
        for row in recent:
            try:
                v = float(row[field])
                if v >= 0:
                    historical_vals.append(v)
            except (KeyError, ValueError):
                pass

        if not historical_vals:
            continue

        baseline = sum(historical_vals) / len(historical_vals)
        if baseline == 0:
            continue

        deviation = (current_val - baseline) / baseline

        # For lower-is-better metrics, a positive deviation is a regression.
        # For higher-is-better metrics, a negative deviation is a regression.
        is_regression = (
            (field in LOWER_IS_BETTER and deviation > threshold) or
            (field not in LOWER_IS_BETTER and deviation < -threshold)
        )

        if is_regression:
            direction = "↑" if deviation > 0 else "↓"
            warnings.append(
                f"REGRESSION {field}: {current_val:.0f} vs baseline "
                f"{baseline:.1f} ({deviation:+.1%}) {direction}"
            )

    return warnings


# ── Chart generation ──────────────────────────────────────────────────────────

def _generate_chart(csv_path: Path, chart_path: Path) -> None:
    """Generate a six-panel time-series chart from the CSV history."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import matplotlib.dates as mdates
    except ImportError:
        print("  WARNING: matplotlib not available — skipping chart generation",
              file=sys.stderr)
        return

    history = _load_history(csv_path)
    if len(history) < 2:
        print("  INFO: fewer than 2 data points — skipping chart generation")
        return

    # Parse timestamps and metric values
    timestamps = []
    data: Dict[str, List[float]] = {f: [] for f in CSV_FIELDS
                                     if f not in ("timestamp", "label")}

    for row in history:
        try:
            ts = datetime.fromisoformat(row["timestamp"].replace("Z", "+00:00"))
            timestamps.append(ts)
        except (KeyError, ValueError):
            continue
        for field in data:
            try:
                v = float(row.get(field, "nan"))
            except ValueError:
                v = float("nan")
            data[field].append(v)

    if not timestamps:
        return

    # Six panels: one per metric (excluding net_rtt_ms if all -1)
    metric_labels = {
        "boot_ms":               "Boot time (ms)",
        "mem_after_boot_bytes":  "Heap after boot (bytes)",
        "ttfb_ms":               "TTFB (ms)",
        "tokens_per_sec":        "Tokens/sec",
        "mem_after_infer_bytes": "Heap after inference (bytes)",
        "net_rtt_ms":            "Net RTT to SearXNG (ms)",
    }

    fig, axes = plt.subplots(2, 3, figsize=(15, 8))
    fig.suptitle("ChimeraOS Performance Metrics Over Time", fontsize=14,
                 fontweight="bold")
    axes_flat = axes.flatten()

    for idx, (field, ylabel) in enumerate(metric_labels.items()):
        ax = axes_flat[idx]
        vals = data[field]

        # Filter out -1 sentinel values for net_rtt_ms
        valid_pairs = [(t, v) for t, v in zip(timestamps, vals)
                       if v >= 0 and v == v]  # v == v excludes NaN
        if not valid_pairs:
            ax.set_title(ylabel)
            ax.text(0.5, 0.5, "No data", transform=ax.transAxes,
                    ha="center", va="center", color="gray")
            continue

        ts_valid, v_valid = zip(*valid_pairs)

        ax.plot(ts_valid, v_valid, "o-", linewidth=1.5, markersize=4,
                color="#2196F3")

        # Rolling mean line (window=10)
        if len(v_valid) >= 3:
            window = min(10, len(v_valid))
            rolling = [
                sum(v_valid[max(0, i - window + 1):i + 1]) /
                len(v_valid[max(0, i - window + 1):i + 1])
                for i in range(len(v_valid))
            ]
            ax.plot(ts_valid, rolling, "--", linewidth=1, color="#FF5722",
                    alpha=0.7, label="10-run mean")
            ax.legend(fontsize=7)

        ax.set_title(ylabel, fontsize=10)
        ax.set_xlabel("Date/Time", fontsize=8)
        ax.set_ylabel(ylabel, fontsize=8)
        ax.xaxis.set_major_formatter(mdates.DateFormatter("%m/%d %H:%M"))
        ax.tick_params(axis="x", rotation=30, labelsize=7)
        ax.tick_params(axis="y", labelsize=7)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    chart_path.parent.mkdir(parents=True, exist_ok=True)
    plt.savefig(str(chart_path), dpi=120, bbox_inches="tight")
    plt.close()
    print(f"  Chart saved: {chart_path}")


# ── Main ──────────────────────────────────────────────────────────────────────

def _parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Collect ChimeraOS performance metrics and append to CSV"
    )
    p.add_argument("--iso",           default=str(DEFAULT_ISO))
    p.add_argument("--csv",           default=str(DEFAULT_CSV))
    p.add_argument("--chart",         default=str(DEFAULT_CHART))
    p.add_argument("--searxng-host",  default="localhost")
    p.add_argument("--searxng-port",  type=int, default=8080)
    p.add_argument("--runs",          type=int, default=1)
    p.add_argument("--timeout",       type=int, default=120)
    p.add_argument("--label",         default="")
    p.add_argument("--no-chart",      action="store_true")
    p.add_argument("--fail-on-regression", action="store_true")
    return p.parse_args()


def _single_run(
    iso_path: str,
    searxng_host: str,
    searxng_port: int,
    timeout: int,
    label: str,
) -> Optional[Dict[str, str]]:
    """Perform a single metrics collection run.  Returns a CSV row dict."""

    # Measure net RTT before booting QEMU (host-side)
    print(f"  Measuring net RTT to {searxng_host}:{searxng_port}...")
    net_rtt = _measure_rtt(searxng_host, searxng_port)
    if net_rtt < 0:
        print(f"  WARNING: {searxng_host}:{searxng_port} unreachable "
              f"— net_rtt_ms will be -1")
    else:
        print(f"  net_rtt_ms = {net_rtt}")

    # Build disk image
    with tempfile.NamedTemporaryFile(suffix=".img", delete=False) as f:
        disk_path = f.name
    try:
        print("  Building disk image...")
        _build_disk(disk_path)

        # Boot QEMU
        print(f"  Booting QEMU (timeout={timeout}s)...")
        lines = _run_qemu(iso_path, disk_path, timeout)
    finally:
        try:
            os.unlink(disk_path)
        except OSError:
            pass

    # Parse metrics
    metrics = _parse_metrics(lines)
    if not metrics:
        print("  ERROR: No [METRICS] lines found in serial log", file=sys.stderr)
        print("  Last 20 serial lines:")
        for ln in lines[-20:]:
            print(f"    {ln}")
        return None

    print(f"  Kernel metrics: {metrics}")

    # Overwrite net_rtt_ms sentinel with host-measured value
    metrics["net_rtt_ms"] = net_rtt

    # Build CSV row
    timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    row: Dict[str, str] = {
        "timestamp":             timestamp,
        "label":                 label,
        "boot_ms":               str(metrics.get("boot_ms", -1)),
        "mem_after_boot_bytes":  str(metrics.get("mem_after_boot_bytes", -1)),
        "ttfb_ms":               str(metrics.get("ttfb_ms", -1)),
        "tokens_per_sec":        str(metrics.get("tokens_per_sec", -1)),
        "mem_after_infer_bytes": str(metrics.get("mem_after_infer_bytes", -1)),
        "net_rtt_ms":            str(net_rtt),
    }
    return row


def main() -> int:
    args = _parse_args()

    iso_path  = args.iso
    csv_path  = Path(args.csv)
    chart_path = Path(args.chart)

    if not os.path.exists(iso_path):
        print(f"ERROR: ISO not found: {iso_path}", file=sys.stderr)
        print("  Run: cd <repo> && make integration-test-iso", file=sys.stderr)
        return 1

    all_warnings: List[str] = []

    for run_idx in range(args.runs):
        print(f"\n{'='*70}")
        print(f"perf_collect.py  run {run_idx + 1}/{args.runs}")
        print(f"{'='*70}")

        row = _single_run(
            iso_path     = iso_path,
            searxng_host = args.searxng_host,
            searxng_port = args.searxng_port,
            timeout      = args.timeout,
            label        = args.label,
        )

        if row is None:
            print(f"  Run {run_idx + 1} failed — skipping CSV append")
            continue

        # Load history before appending (for regression check)
        history = _load_history(csv_path)

        # Append to CSV
        _append_row(csv_path, row)
        print(f"  Appended to CSV: {csv_path}")

        # Regression check
        warnings = _check_regressions(history, row)
        for w in warnings:
            print(f"  ⚠  {w}")
        all_warnings.extend(warnings)

    # Regenerate chart
    if not args.no_chart:
        print(f"\n  Generating regression chart...")
        _generate_chart(csv_path, chart_path)

    # Summary
    print(f"\n{'='*70}")
    print(f"Done.  CSV: {csv_path}")
    if all_warnings:
        print(f"Regressions detected ({len(all_warnings)}):")
        for w in all_warnings:
            print(f"  {w}")
        if args.fail_on_regression:
            return 1
    else:
        print("No regressions detected.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
