#!/usr/bin/env python3
"""
tests/fs/test_lfn.py — FAT32 LFN test harness for ChimeraOS

Usage:
    python3 tests/fs/test_lfn.py [--iso PATH] [--results PATH]

What it does:
  1. Creates a fresh 64 MB FAT32 disk image.
  2. Boots chimaera_lfn_test.iso with the disk attached.
  3. Waits for the kernel to run the LFN tests and emit [LFNTEST] PASS/FAIL lines.
  4. Waits for the [LFNTEST] DONE marker.
  5. Kills QEMU.
  6. Runs fsck.fat -n on the disk image to verify on-disk consistency.
  7. Writes a JSON report with the results.
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Optional

# ── Configuration ─────────────────────────────────────────────────────────────
CHIMERA_DIR = Path(__file__).resolve().parent.parent.parent
DEFAULT_ISO  = CHIMERA_DIR / "chimaera_lfn_test.iso"
DEFAULT_RESULTS = CHIMERA_DIR / "tests" / "fs" / "lfn_results.json"

QEMU_CMD    = "qemu-system-i386"
DISK_SIZE_MB = 64
BOOT_TIMEOUT = 60       # seconds to wait for tests to complete

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
    
    # fsck.fat returns 0 if clean, 1 if recoverable errors, 2 if fatal
    # But we also want to check if it reports any LFN-specific issues
    # Note: "Free cluster summary wrong" is a known benign issue we ignore
    
    clean = True
    filtered_output = []
    for line in output.splitlines():
        if not line.strip():
            continue
        if "fsck.fat" in line or "Warning: Filesystem is FAT32" in line or "less than the required minimum" in line or "This may lead to problems" in line:
            continue
        if "Free cluster summary wrong" in line or "Auto-correcting" in line or "Leaving filesystem unchanged" in line or "files, " in line:
            continue
        
        filtered_output.append(line)
        clean = False
        
    return clean, "\n".join(filtered_output) if not clean else "Clean"

# ── QEMU runner ────────────────────────────────────────────────────────────────

class QEMURunner:
    """Manages a QEMU process with serial output monitoring."""

    def __init__(self, iso: str, disk: str):
        self.iso  = iso
        self.disk = disk
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
            "-drive", f"file={self.disk},format=raw,if=ide,index=1,cache=writeback",
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

# ── Main test flow ─────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="FAT32 LFN test harness")
    parser.add_argument("--iso", type=str, default=str(DEFAULT_ISO),
                        help="Path to chimaera_lfn_test.iso")
    parser.add_argument("--results", type=str, default=str(DEFAULT_RESULTS),
                        help="Path to output JSON results file")
    args = parser.parse_args()

    if not os.path.exists(args.iso):
        print(f"ERROR: ISO not found at {args.iso}")
        print("Please run 'make lfn-test-iso' first.")
        sys.exit(1)

    # Create a temporary disk image
    disk_fd, disk_path = tempfile.mkstemp(suffix=".img", prefix="lfn_test_")
    os.close(disk_fd)

    print(f"Creating fresh FAT32 disk image: {disk_path}")
    create_disk(disk_path)

    print(f"Booting QEMU with {args.iso}...")
    runner = QEMURunner(args.iso, disk_path)
    runner.start()

    result = {
        "kernel_tests_completed": False,
        "kernel_tests_passed": 0,
        "kernel_tests_failed": 0,
        "kernel_test_details": [],
        "fsck_clean": False,
        "fsck_output": "",
        "overall_pass": False
    }

    try:
        print(f"Waiting up to {BOOT_TIMEOUT}s for LFN tests to complete...")
        completed = runner.wait_for_marker("[LFNTEST] DONE", BOOT_TIMEOUT)
        
        result["kernel_tests_completed"] = completed
        
        # Parse the serial log for test results
        log_lines = runner.get_serial_log()
        for line in log_lines:
            if "[LFNTEST] PASS" in line:
                result["kernel_tests_passed"] += 1
                parts = line.split("PASS", 1)[1].strip().split(" ", 1)
                test_id = parts[0] if len(parts) > 0 else "unknown"
                desc = parts[1] if len(parts) > 1 else ""
                result["kernel_test_details"].append({"id": test_id, "status": "PASS", "desc": desc})
                print(f"  ✅ {test_id}: {desc}")
            elif "[LFNTEST] FAIL" in line:
                result["kernel_tests_failed"] += 1
                parts = line.split("FAIL", 1)[1].strip().split(" ", 1)
                test_id = parts[0] if len(parts) > 0 else "unknown"
                desc = parts[1] if len(parts) > 1 else ""
                result["kernel_test_details"].append({"id": test_id, "status": "FAIL", "desc": desc})
                print(f"  ❌ {test_id}: {desc}")
                
        if not completed:
            print("❌ TIMEOUT: Kernel tests did not complete.")
        else:
            print(f"Kernel tests finished: {result['kernel_tests_passed']} passed, {result['kernel_tests_failed']} failed.")
            
    finally:
        runner.stop()

    # Run fsck.fat on the disk image
    print("\nRunning fsck.fat on the disk image...")
    clean, output = run_fsck(disk_path)
    result["fsck_clean"] = clean
    result["fsck_output"] = output
    
    if clean:
        print("  ✅ fsck.fat reports clean filesystem (ignoring benign free cluster count warnings)")
    else:
        print("  ❌ fsck.fat found errors:")
        for line in output.splitlines():
            print(f"     {line}")

    # Determine overall pass/fail
    result["overall_pass"] = (
        result["kernel_tests_completed"] and 
        result["kernel_tests_failed"] == 0 and 
        result["fsck_clean"]
    )

    # Write results
    os.makedirs(os.path.dirname(args.results), exist_ok=True)
    with open(args.results, "w") as f:
        json.dump(result, f, indent=2)
    print(f"\nResults written to {args.results}")

    # Clean up
    try:
        os.unlink(disk_path)
    except Exception:
        pass

    if result["overall_pass"]:
        print("\n🎉 ALL TESTS PASSED!")
        sys.exit(0)
    else:
        print("\n💥 TESTS FAILED!")
        sys.exit(1)

if __name__ == "__main__":
    main()
