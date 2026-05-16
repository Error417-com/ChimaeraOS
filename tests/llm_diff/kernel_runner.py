#!/usr/bin/env python3
"""
kernel_runner.py — ChimeraOS LLM Differential Test Harness: Kernel Side
========================================================================
Boots ChimeraOS in QEMU, waits for the kernel to signal readiness, then
sends LLM_DIFF_TEST <id> commands over the serial port for each prompt.
Captures the structured [LLM_DIFF] output lines and writes kernel_output.json.

Serial protocol (kernel → host):
  [LLM_DIFF_READY]                         — kernel ready to accept commands
  [LLM_DIFF_START] id=<N>                  — kernel started processing prompt N
  [LLM_DIFF] TOP10: id:prob,...             — top-10 logit probs (first token)
  [LLM_DIFF] GREEDY: id1,id2,...           — greedy token IDs (5 tokens)
  [LLM_DIFF] TEXT: <decoded text>          — decoded greedy text
  [LLM_DIFF_END] id=<N>                    — kernel finished prompt N

Usage:
    python3 kernel_runner.py [--iso PATH] [--disk PATH] [--output PATH]
                             [--timeout-per-prompt SECONDS]
                             [--prompt-ids 0,1,2,...]
"""
import argparse
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time

SCRIPT_DIR   = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT    = os.path.dirname(os.path.dirname(SCRIPT_DIR))
CHIMERA_DIR  = os.path.join(REPO_ROOT, "chimera_os")

DEFAULT_ISO    = os.path.join(CHIMERA_DIR, "chimaera.iso")
DEFAULT_DISK   = os.path.join(CHIMERA_DIR, "chimaera.img")
DEFAULT_OUTPUT = os.path.join(SCRIPT_DIR, "kernel_output.json")
PROMPTS_FILE   = os.path.join(SCRIPT_DIR, "prompts.json")

BOOT_TIMEOUT           = 300   # seconds to wait for [LLM_DIFF_READY]
DEFAULT_PROMPT_TIMEOUT = 600   # seconds per prompt (model is slow on i386)


def launch_qemu(iso: str, disk: str, serial_log: str) -> subprocess.Popen:
    """Start QEMU with serial output redirected to a file."""
    cmd = [
        "qemu-system-i386",
        "-cdrom",   iso,
        "-drive",   f"file={disk},format=raw,if=ide,index=1",
        "-m",       "1024M",
        "-serial",  f"file:{serial_log}",
        "-display", "none",
        "-no-reboot",
        "-no-shutdown",
        "-net",     "nic,model=rtl8139",
        "-net",     "user",
    ]
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)


def tail_log(path: str, stop_event: threading.Event,
             lines: list) -> None:
    """Background thread: continuously append new lines from the log file."""
    pos = 0
    while not stop_event.is_set():
        try:
            with open(path, "r", errors="replace") as f:
                f.seek(pos)
                chunk = f.read()
                if chunk:
                    for line in chunk.splitlines(keepends=True):
                        lines.append(line.rstrip("\n\r"))
                    pos = f.tell()
        except FileNotFoundError:
            pass
        time.sleep(0.2)


def wait_for_marker(lines: list, marker: str, timeout: float,
                    start_idx: int = 0) -> int:
    """
    Wait until a line containing `marker` appears in `lines[start_idx:]`.
    Returns the index of the matching line, or -1 on timeout.
    """
    deadline = time.monotonic() + timeout
    idx = start_idx
    while time.monotonic() < deadline:
        while idx < len(lines):
            if marker in lines[idx]:
                return idx
            idx += 1
        time.sleep(0.5)
    return -1


def parse_top10(line: str):
    """Parse '[LLM_DIFF] TOP10: id:prob,...' → list of {id, prob}."""
    m = re.search(r"TOP10:\s*(.+)$", line)
    if not m:
        return []
    tokens = []
    for part in m.group(1).split(","):
        part = part.strip()
        if ":" in part:
            tid, prob = part.split(":", 1)
            try:
                tokens.append({"id": int(tid.strip()),
                                "prob": float(prob.strip())})
            except ValueError:
                pass
    return tokens


def parse_greedy(line: str):
    """Parse '[LLM_DIFF] GREEDY: id1,id2,...' → list of int."""
    m = re.search(r"GREEDY:\s*(.+)$", line)
    if not m:
        return []
    ids = []
    for part in m.group(1).split(","):
        part = part.strip()
        try:
            ids.append(int(part))
        except ValueError:
            pass
    return ids


def parse_text(line: str) -> str:
    """Parse '[LLM_DIFF] TEXT: ...' → decoded text."""
    m = re.search(r"TEXT:\s*(.*)$", line)
    return m.group(1) if m else ""


def run_kernel(iso: str, disk: str, output_path: str,
               prompt_ids: list, prompt_timeout: int) -> None:
    with open(PROMPTS_FILE, "r", encoding="utf-8") as f:
        all_prompts = json.load(f)

    if prompt_ids:
        prompts = [p for p in all_prompts if p["id"] in prompt_ids]
    else:
        prompts = all_prompts

    serial_log = tempfile.mktemp(suffix=".log", prefix="chimera_diff_")
    print(f"[kernel_runner] Serial log: {serial_log}")
    print(f"[kernel_runner] Booting QEMU: {iso}")

    proc = launch_qemu(iso, disk, serial_log)
    lines: list = []
    stop_evt = threading.Event()
    reader = threading.Thread(target=tail_log,
                              args=(serial_log, stop_evt, lines),
                              daemon=True)
    reader.start()

    try:
        print(f"[kernel_runner] Waiting for [LLM_DIFF_READY] "
              f"(up to {BOOT_TIMEOUT}s)...")
        ready_idx = wait_for_marker(lines, "[LLM_DIFF_READY]", BOOT_TIMEOUT)
        if ready_idx < 0:
            print("[kernel_runner] ERROR: kernel never signalled LLM_DIFF_READY",
                  file=sys.stderr)
            print("[kernel_runner] Last 20 serial lines:")
            for l in lines[-20:]:
                print("  ", l)
            sys.exit(1)
        print(f"[kernel_runner] Kernel ready (line {ready_idx})")

        # Open a named pipe / PTY to send commands.
        # Since we used -serial file:, we can't send commands back.
        # Instead we use a second QEMU instance with -serial stdio piped,
        # but the simpler approach for a file-based serial is to use
        # the kernel's auto-run mode: the kernel reads prompt IDs from a
        # pre-baked list and runs them sequentially, printing results.
        # The [LLM_DIFF_READY] line is followed by the kernel running all
        # prompts automatically (see kernel.c ENABLE_LLM_DIFF section).
        # We just need to collect the output.

        results = []
        scan_from = ready_idx + 1

        for p in prompts:
            pid = p["id"]
            print(f"[kernel_runner] Waiting for prompt {pid:02d} "
                  f"(timeout={prompt_timeout}s)...")

            start_marker = f"[LLM_DIFF_START] id={pid}"
            end_marker   = f"[LLM_DIFF_END] id={pid}"

            start_idx = wait_for_marker(lines, start_marker,
                                        prompt_timeout, scan_from)
            if start_idx < 0:
                print(f"[kernel_runner] TIMEOUT waiting for prompt {pid}",
                      file=sys.stderr)
                results.append({
                    "id": pid, "error": "timeout",
                    "top_10_tokens": [], "top_1_token_id": -1,
                    "top_1_prob": 0.0, "greedy_tokens": [],
                    "greedy_text": "",
                })
                continue

            end_idx = wait_for_marker(lines, end_marker,
                                      prompt_timeout, start_idx)
            if end_idx < 0:
                print(f"[kernel_runner] TIMEOUT waiting for end of prompt {pid}",
                      file=sys.stderr)
                end_idx = len(lines)

            # Parse the block between start and end
            block = lines[start_idx:end_idx + 1]
            top10   = []
            greedy  = []
            text    = ""
            for line in block:
                if "[LLM_DIFF] TOP10:" in line:
                    top10  = parse_top10(line)
                elif "[LLM_DIFF] GREEDY:" in line:
                    greedy = parse_greedy(line)
                elif "[LLM_DIFF] TEXT:" in line:
                    text   = parse_text(line)

            top1_id   = top10[0]["id"]   if top10 else -1
            top1_prob = top10[0]["prob"] if top10 else 0.0

            result = {
                "id":             pid,
                "description":    p["description"],
                "category":       p["category"],
                "user_text":      p["user_text"],
                "top_10_tokens":  top10,
                "top_1_token_id": top1_id,
                "top_1_prob":     top1_prob,
                "greedy_tokens":  greedy,
                "greedy_text":    text,
            }
            results.append(result)
            scan_from = end_idx + 1

            print(f"           top-1={top1_id} ({top1_prob:.4f}), "
                  f"greedy={greedy}")

        with open(output_path, "w", encoding="utf-8") as f:
            json.dump(results, f, indent=2, ensure_ascii=False)
        print(f"\n[kernel_runner] Wrote {len(results)} results → {output_path}")

    finally:
        stop_evt.set()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        # Clean up serial log
        try:
            os.unlink(serial_log)
        except OSError:
            pass


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Run ChimeraOS kernel LLM diff tests via QEMU serial")
    ap.add_argument("--iso",    default=DEFAULT_ISO,
                    help="Path to chimaera.iso")
    ap.add_argument("--disk",   default=DEFAULT_DISK,
                    help="Path to chimaera.img (FAT32 disk with model)")
    ap.add_argument("--output", default=DEFAULT_OUTPUT,
                    help="Output JSON path (default: kernel_output.json)")
    ap.add_argument("--timeout-per-prompt", type=int,
                    default=DEFAULT_PROMPT_TIMEOUT,
                    help="Seconds to wait per prompt (default: 600)")
    ap.add_argument("--prompt-ids", default=None,
                    help="Comma-separated prompt IDs to run (default: all)")
    args = ap.parse_args()

    ids = None
    if args.prompt_ids:
        ids = [int(x.strip()) for x in args.prompt_ids.split(",")]

    run_kernel(args.iso, args.disk, args.output,
               ids or [], args.timeout_per_prompt)


if __name__ == "__main__":
    main()
