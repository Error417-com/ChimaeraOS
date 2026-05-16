#!/usr/bin/env python3
"""
tests/fs/powerfail.py — FAT32 power-fail test harness for ChimeraOS

Usage:
    python3 tests/fs/powerfail.py [--iterations N] [--iso PATH] [--results DIR]

What it does (per iteration):
  1. Create a fresh 64 MB FAT32 disk image.
  2. Boot chimaera_powerfail_test.iso with the disk attached.
  3. Wait for "[PFTEST] WRITE_START" marker on serial.
  4. Kill QEMU with SIGKILL after a random delay (0–800 ms) — simulates power cut.
  5. RECOVERY BOOT: reboot the same disk image, wait for "[INIT] Persistent disk OK"
     so that fat32_fsck_repair() runs and cleans up any orphans/FAT2 mismatches.
  6. Kill QEMU again (recovery boot complete).
  7. Run fsck.fat -n on the post-recovery disk image.
  8. Run our Python FAT32 consistency checker (FAT1==FAT2, orphan clusters,
     dir-entry size vs chain length).
  9. Record any inconsistencies to results/iteration_NNN.json.

After all iterations, print a summary of the top corruption patterns.
"""

import argparse
import json
import os
import random
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Optional

# ── Configuration ─────────────────────────────────────────────────────────────
CHIMERA_DIR = Path(__file__).resolve().parent.parent.parent
DEFAULT_ISO  = CHIMERA_DIR / "chimaera_powerfail_test.iso"
DEFAULT_RESULTS = CHIMERA_DIR / "tests" / "fs" / "powerfail_results"

QEMU_CMD    = "qemu-system-i386"
DISK_SIZE_MB = 64
BOOT_TIMEOUT = 60       # seconds to wait for PFTEST READY before giving up
RECOVERY_TIMEOUT = 30   # seconds to wait for recovery boot to complete
KILL_DELAY_MIN_MS = 0   # minimum ms after WRITE_START before SIGKILL
KILL_DELAY_MAX_MS = 800 # maximum ms after WRITE_START before SIGKILL

# ── FAT32 constants ────────────────────────────────────────────────────────────
FAT32_EOC_MIN  = 0x0FFFFFF8
FAT32_EOC_MAX  = 0x0FFFFFFF
FAT32_FREE     = 0x00000000
FAT32_BAD      = 0x0FFFFFF7
FAT32_MASK     = 0x0FFFFFFF

# ── Disk image helpers ─────────────────────────────────────────────────────────

def create_disk(path: str) -> None:
    """Create a fresh 64 MB FAT32 disk image."""
    with open(path, "wb") as f:
        f.seek(DISK_SIZE_MB * 1024 * 1024 - 1)
        f.write(b'\x00')
    subprocess.run(
        ["mkfs.fat", "-F", "32", "-n", "CHIMERA", path],
        check=True, capture_output=True
    )


def run_fsck(path: str) -> tuple:
    """Run fsck.fat -n on the image. Returns (clean, output)."""
    result = subprocess.run(
        ["fsck.fat", "-n", path],
        capture_output=True, text=True
    )
    output = result.stdout + result.stderr
    clean = result.returncode == 0
    return clean, output


# ── FAT32 Python consistency checker ──────────────────────────────────────────

class FAT32Image:
    """Minimal FAT32 image reader for consistency checking."""

    def __init__(self, path: str):
        self.path = path
        self.f = open(path, "rb")
        self._parse_bpb()

    def close(self):
        self.f.close()

    def _read(self, offset: int, size: int) -> bytes:
        self.f.seek(offset)
        return self.f.read(size)

    def _parse_bpb(self):
        bpb = self._read(0, 512)
        if len(bpb) < 512:
            raise ValueError("Image too small to contain a BPB")

        self.bytes_per_sector    = struct.unpack_from("<H", bpb, 11)[0]
        self.sectors_per_cluster = struct.unpack_from("<B", bpb, 13)[0]
        self.reserved_sectors    = struct.unpack_from("<H", bpb, 14)[0]
        self.num_fats            = struct.unpack_from("<B", bpb, 16)[0]
        self.total_sectors_16    = struct.unpack_from("<H", bpb, 19)[0]
        self.fat_size_16         = struct.unpack_from("<H", bpb, 22)[0]
        self.total_sectors_32    = struct.unpack_from("<I", bpb, 32)[0]
        self.fat_size_32         = struct.unpack_from("<I", bpb, 36)[0]
        self.root_cluster        = struct.unpack_from("<I", bpb, 44)[0]

        self.fat_size = self.fat_size_32 if self.fat_size_16 == 0 else self.fat_size_16
        self.total_sectors = (self.total_sectors_32 if self.total_sectors_16 == 0
                              else self.total_sectors_16)

        self.fat1_offset = self.reserved_sectors * self.bytes_per_sector
        self.fat2_offset = self.fat1_offset + self.fat_size * self.bytes_per_sector
        self.data_offset = (self.reserved_sectors + self.num_fats * self.fat_size) * self.bytes_per_sector
        self.cluster_size = self.sectors_per_cluster * self.bytes_per_sector

        data_sectors = self.total_sectors - (self.reserved_sectors + self.num_fats * self.fat_size)
        self.total_clusters = data_sectors // self.sectors_per_cluster

    def _fat_entry(self, fat_offset: int, cluster: int) -> int:
        offset = fat_offset + cluster * 4
        data = self._read(offset, 4)
        if len(data) < 4:
            return FAT32_FREE
        val = struct.unpack_from("<I", data)[0]
        return val & FAT32_MASK

    def fat1(self, cluster: int) -> int:
        return self._fat_entry(self.fat1_offset, cluster)

    def fat2(self, cluster: int) -> int:
        return self._fat_entry(self.fat2_offset, cluster)

    def cluster_data_offset(self, cluster: int) -> int:
        return self.data_offset + (cluster - 2) * self.cluster_size

    def read_cluster(self, cluster: int) -> bytes:
        offset = self.cluster_data_offset(cluster)
        return self._read(offset, self.cluster_size)

    def is_eoc(self, val: int) -> bool:
        return val >= FAT32_EOC_MIN

    def is_free(self, val: int) -> bool:
        return val == FAT32_FREE

    def is_bad(self, val: int) -> bool:
        return val == FAT32_BAD

    def follow_chain(self, start: int) -> list:
        chain = []
        seen = set()
        cluster = start
        while not self.is_eoc(cluster) and not self.is_free(cluster):
            if cluster in seen or cluster < 2 or cluster >= self.total_clusters + 2:
                break
            seen.add(cluster)
            chain.append(cluster)
            cluster = self.fat1(cluster)
        return chain

    def scan_directory(self, cluster: int, depth: int = 0) -> list:
        entries = []
        if depth > 10:
            return entries
        chain = self.follow_chain(cluster)
        for c in chain:
            data = self.read_cluster(c)
            for i in range(0, len(data), 32):
                entry = data[i:i+32]
                if len(entry) < 32:
                    break
                first_byte = entry[0]
                if first_byte == 0x00:
                    break
                if first_byte == 0xE5:
                    continue
                attr = entry[11]
                if attr == 0x0F:
                    continue
                if attr & 0x08:
                    continue
                name = entry[0:8].rstrip(b' ').decode('ascii', errors='replace')
                ext  = entry[8:11].rstrip(b' ').decode('ascii', errors='replace')
                full_name = name + ('.' + ext if ext else '')
                hi = struct.unpack_from("<H", entry, 20)[0]
                lo = struct.unpack_from("<H", entry, 26)[0]
                start_cluster = (hi << 16) | lo
                file_size = struct.unpack_from("<I", entry, 28)[0]
                is_dir = bool(attr & 0x10)
                entries.append({
                    "name": full_name,
                    "start_cluster": start_cluster,
                    "file_size": file_size,
                    "is_dir": is_dir,
                    "attr": attr,
                })
        return entries


def check_consistency(disk_path: str) -> dict:
    """
    Run our Python FAT32 consistency checker.
    Returns a dict with fat_mismatch, orphaned_clusters, size_chain_mismatch, errors.
    """
    result = {
        "fat_mismatch": [],
        "orphaned_clusters": [],
        "size_chain_mismatch": [],
        "errors": [],
        "total_clusters": 0,
        "used_clusters": 0,
    }

    try:
        img = FAT32Image(disk_path)
    except Exception as e:
        result["errors"].append(f"Failed to parse FAT32 image: {e}")
        return result

    try:
        total = img.total_clusters
        result["total_clusters"] = total

        # Step 1: FAT1 vs FAT2 comparison
        for c in range(2, total + 2):
            f1 = img.fat1(c)
            f2 = img.fat2(c)
            if f1 != f2:
                result["fat_mismatch"].append((c, f1, f2))

        # Step 2: Build reachable cluster set from root directory
        reachable = set()
        dir_queue = [img.root_cluster]
        visited_dirs = set()
        all_entries = []

        while dir_queue:
            dir_cluster = dir_queue.pop(0)
            if dir_cluster in visited_dirs or dir_cluster < 2:
                continue
            visited_dirs.add(dir_cluster)
            entries = img.scan_directory(dir_cluster)
            for e in entries:
                sc = e["start_cluster"]
                if sc < 2:
                    continue
                chain = img.follow_chain(sc)
                for c in chain:
                    reachable.add(c)
                all_entries.append(e)
                if e["is_dir"] and e["name"] not in (".", ".."):
                    dir_queue.append(sc)

        for c in img.follow_chain(img.root_cluster):
            reachable.add(c)

        # Step 3: Orphaned clusters
        used = 0
        for c in range(2, total + 2):
            f1 = img.fat1(c)
            if not img.is_free(f1) and not img.is_bad(f1):
                used += 1
                if c not in reachable:
                    result["orphaned_clusters"].append(c)
        result["used_clusters"] = used

        # Step 4: Dir-entry file size vs chain length
        for e in all_entries:
            if e["is_dir"]:
                continue
            sc = e["start_cluster"]
            if sc < 2:
                if e["file_size"] > 0:
                    result["size_chain_mismatch"].append(
                        (e["name"], e["file_size"], 0)
                    )
                continue
            chain = img.follow_chain(sc)
            chain_bytes = len(chain) * img.cluster_size
            if e["file_size"] > chain_bytes:
                result["size_chain_mismatch"].append(
                    (e["name"], e["file_size"], chain_bytes)
                )
            elif e["file_size"] > 0 and chain_bytes > e["file_size"] + img.cluster_size:
                result["size_chain_mismatch"].append(
                    (e["name"], e["file_size"], chain_bytes)
                )

    except Exception as e:
        result["errors"].append(f"Checker exception: {e}")
    finally:
        img.close()

    return result


# ── QEMU runner ────────────────────────────────────────────────────────────────

class QEMURunner:
    """Manages a QEMU process with serial output monitoring."""

    def __init__(self, iso: str, disk: str, cache_mode: str = "writeback"):
        self.iso  = iso
        self.disk = disk
        self.cache_mode = cache_mode
        self.proc: Optional[subprocess.Popen] = None
        self.serial_lines: list = []
        self._lock = threading.Lock()
        self._reader_thread: Optional[threading.Thread] = None
        self._stop_reader = False
        self._serial_path = ""

    def start(self):
        """Start QEMU with a file-backed serial port."""
        self.serial_lines = []
        self._stop_reader = False

        serial_file = tempfile.NamedTemporaryFile(
            suffix=".serial", delete=False, mode="wb"
        )
        serial_file.close()
        self._serial_path = serial_file.name

        cmd = [
            QEMU_CMD,
            "-cdrom", self.iso,
            "-drive", f"file={self.disk},format=raw,if=ide,index=1,cache={self.cache_mode}",
            "-m", "128M",
            "-boot", "d",
            "-serial", f"file:{self._serial_path}",
            "-display", "none",
            "-no-reboot",
            "-no-shutdown",
        ]

        self.proc = subprocess.Popen(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self._reader_thread = threading.Thread(
            target=self._read_serial, daemon=True
        )
        self._reader_thread.start()

    def _read_serial(self):
        """Background thread: tail the serial file for new lines."""
        buf = b""
        pos = 0
        while not self._stop_reader:
            try:
                with open(self._serial_path, "rb") as f:
                    f.seek(pos)
                    data = f.read(4096)
                    if data:
                        pos += len(data)
                        buf += data
                        while b"\n" in buf:
                            line, buf = buf.split(b"\n", 1)
                            decoded = line.decode("ascii", errors="replace").strip()
                            with self._lock:
                                self.serial_lines.append(decoded)
            except Exception:
                pass
            time.sleep(0.01)

    def wait_for_marker(self, marker: str, timeout: float) -> bool:
        """Block until a serial line containing `marker` appears."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self._lock:
                for line in self.serial_lines:
                    if marker in line:
                        return True
            time.sleep(0.02)
        return False

    def kill(self):
        """Send SIGKILL to QEMU (simulates power cut)."""
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL)

    def stop(self):
        """Stop the reader thread and clean up."""
        self._stop_reader = True
        if self._reader_thread:
            self._reader_thread.join(timeout=2)
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(timeout=5)
        try:
            os.unlink(self._serial_path)
        except Exception:
            pass

    def get_serial_log(self) -> list:
        with self._lock:
            return list(self.serial_lines)


# ── Single iteration ───────────────────────────────────────────────────────────

def run_iteration(
    iteration: int,
    iso: str,
    results_dir: Path,
    kill_delay_ms: Optional[int] = None,
) -> dict:
    """
    Run one power-fail iteration.

    Phase 1 — Power-fail boot:
      Boot kernel, wait for WRITE_START, kill after random delay.

    Phase 2 — Recovery boot:
      Reboot the same disk image. The kernel runs fat32_fsck_repair() on mount.
      Wait for [INIT] Persistent disk OK, then kill.

    Phase 3 — Consistency check:
      Run fsck.fat -n and our Python checker on the post-recovery disk image.
    """
    disk_path = str(results_dir / f"disk_{iteration:04d}.img")

    create_disk(disk_path)

    if kill_delay_ms is None:
        kill_delay_ms = random.randint(KILL_DELAY_MIN_MS, KILL_DELAY_MAX_MS)

    result = {
        "iteration": iteration,
        "kill_delay_ms": kill_delay_ms,
        "boot_reached_ready": False,
        "write_start_seen": False,
        "write_done_seen": False,
        "killed_during_write": False,
        "recovery_boot_ok": False,
        "fsck_clean": False,
        "fsck_output": "",
        "py_fat_mismatch": 0,
        "py_orphaned_clusters": 0,
        "py_size_chain_mismatch": 0,
        "py_errors": [],
        "consistent": False,
        "serial_log": [],
        "recovery_serial_log": [],
    }

    # ── Phase 1: Power-fail boot ──────────────────────────────────────────────
    qemu = QEMURunner(iso, disk_path)
    try:
        qemu.start()

        if not qemu.wait_for_marker("[PFTEST] READY", BOOT_TIMEOUT):
            result["serial_log"] = qemu.get_serial_log()
            return result
        result["boot_reached_ready"] = True

        if not qemu.wait_for_marker("[PFTEST] WRITE_START", 10):
            result["serial_log"] = qemu.get_serial_log()
            return result
        result["write_start_seen"] = True

        with qemu._lock:
            done_seen = any("[PFTEST] WRITE_DONE" in l for l in qemu.serial_lines)
        result["write_done_seen"] = done_seen

        # Sleep for the random kill delay, then SIGKILL
        time.sleep(kill_delay_ms / 1000.0)
        qemu.kill()
        result["killed_during_write"] = True

        try:
            qemu.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.proc.kill()
            qemu.proc.wait()

        result["serial_log"] = qemu.get_serial_log()

    finally:
        qemu.stop()

    # ── Phase 2: Recovery boot ────────────────────────────────────────────────
    # Reboot the same (possibly corrupt) disk. The kernel's fat32_mount() runs
    # fat32_fsck_repair() which: (a) copies FAT1→FAT2 for any mismatched sectors,
    # (b) frees orphaned clusters, (c) updates the FSInfo free-cluster count.
    #
    # Use cache=writethrough so that all ATA writes from the kernel are
    # immediately committed to the disk image file. Without this, QEMU's
    # writeback cache may buffer the repair writes and lose them when we
    # kill QEMU with SIGKILL after the recovery boot.
    qemu2 = QEMURunner(iso, disk_path, cache_mode="writethrough")
    try:
        qemu2.start()

        # Wait for "[PFTEST] RECOVERY_HALT" which is printed by the kernel
        # when it detects this is a recovery boot (pftest files already exist).
        # By this point, fat32_fsck_repair() has run and all repair writes
        # are committed (cache=writethrough). The kernel then halts the CPU
        # so no new writes can corrupt the repaired disk.
        recovery_ok = qemu2.wait_for_marker("[PFTEST] RECOVERY_HALT", RECOVERY_TIMEOUT)
        result["recovery_boot_ok"] = recovery_ok
        qemu2.kill()

        try:
            qemu2.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu2.proc.kill()
            qemu2.proc.wait()

        result["recovery_serial_log"] = qemu2.get_serial_log()

    finally:
        qemu2.stop()

    # ── Phase 3: Consistency checks on post-recovery disk ────────────────────

    # 1. fsck.fat
    try:
        fsck_clean, fsck_out = run_fsck(disk_path)
        result["fsck_clean"] = fsck_clean
        result["fsck_output"] = fsck_out[:2000]
    except Exception as e:
        result["fsck_output"] = f"fsck error: {e}"

    # 2. Python checker
    try:
        py_result = check_consistency(disk_path)
        result["py_fat_mismatch"]       = len(py_result["fat_mismatch"])
        result["py_orphaned_clusters"]  = len(py_result["orphaned_clusters"])
        result["py_size_chain_mismatch"]= len(py_result["size_chain_mismatch"])
        result["py_errors"]             = py_result["errors"]
        result["py_total_clusters"]     = py_result["total_clusters"]
        result["py_used_clusters"]      = py_result["used_clusters"]
        if py_result["fat_mismatch"]:
            result["py_fat_mismatch_samples"] = [
                {"cluster": c, "fat1": f1, "fat2": f2}
                for c, f1, f2 in py_result["fat_mismatch"][:10]
            ]
        if py_result["orphaned_clusters"]:
            result["py_orphaned_samples"] = py_result["orphaned_clusters"][:20]
        if py_result["size_chain_mismatch"]:
            result["py_size_mismatch_samples"] = [
                {"name": n, "dir_size": ds, "chain_bytes": cb}
                for n, ds, cb in py_result["size_chain_mismatch"][:10]
            ]
    except Exception as e:
        result["py_errors"].append(f"py_checker exception: {e}")

    # Overall consistency verdict
    result["consistent"] = (
        result["fsck_clean"]
        and result["py_fat_mismatch"] == 0
        and result["py_orphaned_clusters"] == 0
        and result["py_size_chain_mismatch"] == 0
        and not result["py_errors"]
    )

    # Clean up disk image
    try:
        os.unlink(disk_path)
    except Exception:
        pass

    return result


# ── Main ───────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="FAT32 power-fail test harness")
    parser.add_argument("--iterations", type=int, default=100,
                        help="Number of iterations (default: 100)")
    parser.add_argument("--iso", type=str, default=str(DEFAULT_ISO),
                        help="Path to chimaera_powerfail_test.iso")
    parser.add_argument("--results", type=str, default=str(DEFAULT_RESULTS),
                        help="Directory to store per-iteration JSON results")
    parser.add_argument("--keep-disks", action="store_true",
                        help="Keep disk images after each iteration")
    args = parser.parse_args()

    iso = args.iso
    results_dir = Path(args.results)
    results_dir.mkdir(parents=True, exist_ok=True)

    if not os.path.exists(iso):
        print(f"ERROR: ISO not found: {iso}", file=sys.stderr)
        sys.exit(1)

    if not shutil.which("qemu-system-i386"):
        print("ERROR: qemu-system-i386 not found", file=sys.stderr)
        sys.exit(1)

    if not shutil.which("mkfs.fat"):
        print("ERROR: mkfs.fat not found (install dosfstools)", file=sys.stderr)
        sys.exit(1)

    if not shutil.which("fsck.fat"):
        print("ERROR: fsck.fat not found (install dosfstools)", file=sys.stderr)
        sys.exit(1)

    print(f"FAT32 power-fail test: {args.iterations} iterations")
    print(f"  ISO:     {iso}")
    print(f"  Results: {results_dir}")
    print(f"  Mode:    kill-then-recovery-boot (fat32_fsck_repair tested)")
    print()

    all_results = []
    consistent_count = 0
    boot_fail_count = 0
    recovery_fail_count = 0

    corruption_counts = {
        "fat_mismatch": 0,
        "orphaned_clusters": 0,
        "size_chain_mismatch": 0,
        "fsck_failures": 0,
    }

    start_time = time.monotonic()

    for i in range(1, args.iterations + 1):
        iter_start = time.monotonic()
        r = run_iteration(i, iso, results_dir)
        iter_elapsed = time.monotonic() - iter_start
        total_elapsed = time.monotonic() - start_time

        all_results.append(r)

        if r["consistent"]:
            consistent_count += 1
        if not r["boot_reached_ready"]:
            boot_fail_count += 1
        if not r["recovery_boot_ok"]:
            recovery_fail_count += 1

        if r["py_fat_mismatch"] > 0:
            corruption_counts["fat_mismatch"] += 1
        if r["py_orphaned_clusters"] > 0:
            corruption_counts["orphaned_clusters"] += 1
        if r["py_size_chain_mismatch"] > 0:
            corruption_counts["size_chain_mismatch"] += 1
        if not r["fsck_clean"]:
            corruption_counts["fsck_failures"] += 1

        # Build status string
        if r["consistent"]:
            status = "OK"
        else:
            parts = []
            if not r["fsck_clean"]:
                parts.append("fsck")
            if r["py_fat_mismatch"] > 0:
                parts.append(f"FAT_mismatch({r['py_fat_mismatch']})")
            if r["py_orphaned_clusters"] > 0:
                parts.append(f"orphans({r['py_orphaned_clusters']})")
            if r["py_size_chain_mismatch"] > 0:
                parts.append(f"size_mismatch({r['py_size_chain_mismatch']})")
            if r["py_errors"]:
                parts.append("py_err")
            if not r["recovery_boot_ok"]:
                parts.append("recovery_fail")
            status = "CORRUPT[" + ",".join(parts) + "]"

        print(f"[{i:3d}/{args.iterations}] kill={r['kill_delay_ms']:4d}ms  "
              f"{status:<45s} {iter_elapsed:.1f}s  (total {total_elapsed:.0f}s)")

        # Save per-iteration result
        result_file = results_dir / f"iteration_{i:04d}.json"
        with open(result_file, "w") as f:
            json.dump(r, f, indent=2)

    # ── Summary ───────────────────────────────────────────────────────────────
    print()
    print("=" * 70)
    print(f"SUMMARY: {args.iterations} iterations")
    print("=" * 70)
    print(f"  Consistent (no corruption):  {consistent_count}/{args.iterations}")
    print(f"  Inconsistent (corruption):   {args.iterations - consistent_count}/{args.iterations}")
    print(f"  Boot failures:               {boot_fail_count}/{args.iterations}")
    print(f"  Recovery boot failures:      {recovery_fail_count}/{args.iterations}")
    print("Corruption patterns (post-recovery, may overlap):")
    print(f"  FAT1/FAT2 mismatch:          {corruption_counts['fat_mismatch']}/{args.iterations}")
    print(f"  Orphaned clusters:           {corruption_counts['orphaned_clusters']}/{args.iterations}")
    print(f"  Dir-entry size mismatch:     {corruption_counts['size_chain_mismatch']}/{args.iterations}")
    print(f"  fsck.fat failures:           {corruption_counts['fsck_failures']}/{args.iterations}")

    # Rank top 3 issues
    issues = [
        ("Orphaned clusters (cluster allocated but dir entry not written)",
         corruption_counts["orphaned_clusters"]),
        ("FAT1/FAT2 mismatch (FAT2 mirror not written atomically)",
         corruption_counts["fat_mismatch"]),
        ("Dir-entry size mismatch (size committed before chain finalised)",
         corruption_counts["size_chain_mismatch"]),
    ]
    issues.sort(key=lambda x: x[1], reverse=True)
    print("Top 3 issues by frequency (post-recovery):")
    for rank, (desc, count) in enumerate(issues[:3], 1):
        pct = count / args.iterations * 100
        print(f"  #{rank}: {desc}")
        print(f"       Frequency: {count}/{args.iterations} ({pct:.1f}%)")

    # Save summary JSON
    summary = {
        "iterations": args.iterations,
        "consistent": consistent_count,
        "inconsistent": args.iterations - consistent_count,
        "boot_failures": boot_fail_count,
        "recovery_failures": recovery_fail_count,
        "corruption_counts": corruption_counts,
        "top_issues": [{"description": d, "count": c} for d, c in issues],
    }
    summary_file = results_dir / "summary.json"
    with open(summary_file, "w") as f:
        json.dump(summary, f, indent=2)

    print(f"Detailed results: {results_dir}/")
    print(f"Summary JSON:     {summary_file}")

    # Exit with non-zero if any corruption found
    sys.exit(0 if consistent_count == args.iterations else 1)


if __name__ == "__main__":
    main()
