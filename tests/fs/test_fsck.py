#!/usr/bin/env python3
"""
tests/fs/test_fsck.py — ChimeraFS boot-time fsck test harness

Usage:
    python3 tests/fs/test_fsck.py [--iso PATH] [--results PATH]

What it does:
  For each corruption scenario (C0–C5):
    1. Creates a fresh 64 MB FAT32 disk image.
    2. Writes a known set of files to the disk.
    3. Injects a specific corruption into the raw disk image.
    4. Boots chimaera_fsck_test.iso with the corrupted disk.
    5. Waits for [FSCKTEST] PASS/FAIL/DONE markers on serial.
    6. Verifies fsck.fat reports a clean filesystem after repair.
    7. Records the result.

Corruption scenarios:
    C0 — Clean disk (baseline, no corruption)
    C1 — FAT2 sector corrupted (FAT mismatch)
    C2 — Orphaned cluster chain (allocated in FAT, no dir entry)
    C3 — Cross-linked files (two dir entries share a cluster)
    C4 — Invalid directory entry (attr=0xFF)
    C5 — Orphaned LFN entries (LFN without matching 8.3)
"""

import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Optional, Tuple

# ── Configuration ──────────────────────────────────────────────────────────────
CHIMERA_DIR   = Path(__file__).resolve().parent.parent.parent
DEFAULT_ISO   = CHIMERA_DIR / "chimaera_fsck_test.iso"
DEFAULT_RESULTS = CHIMERA_DIR / "tests" / "fs" / "fsck_results.json"

QEMU_CMD     = "qemu-system-i386"
DISK_SIZE_MB = 64
BOOT_TIMEOUT = 90   # seconds

# ── FAT32 geometry helpers ─────────────────────────────────────────────────────

def parse_bpb(disk_path: str) -> dict:
    """Parse the FAT32 BPB from a raw disk image."""
    with open(disk_path, "rb") as f:
        boot = f.read(512)
    bpb = {}
    bpb["bytes_per_sector"]    = struct.unpack_from("<H", boot, 11)[0]
    bpb["sectors_per_cluster"] = struct.unpack_from("<B", boot, 13)[0]
    bpb["reserved_sectors"]    = struct.unpack_from("<H", boot, 14)[0]
    bpb["num_fats"]            = struct.unpack_from("<B", boot, 16)[0]
    bpb["fat_size_32"]         = struct.unpack_from("<I", boot, 36)[0]
    bpb["root_cluster"]        = struct.unpack_from("<I", boot, 44)[0]
    bpb["total_sectors_32"]    = struct.unpack_from("<I", boot, 32)[0]
    bpb["fat1_lba"]  = bpb["reserved_sectors"]
    bpb["fat2_lba"]  = bpb["reserved_sectors"] + bpb["fat_size_32"]
    bpb["data_lba"]  = bpb["reserved_sectors"] + bpb["num_fats"] * bpb["fat_size_32"]
    bpb["sec_size"]  = bpb["bytes_per_sector"]
    return bpb


def lba_to_offset(lba: int, sec_size: int = 512) -> int:
    return lba * sec_size


def cluster_to_lba(cluster: int, bpb: dict) -> int:
    return bpb["data_lba"] + (cluster - 2) * bpb["sectors_per_cluster"]


def read_fat_entry(disk_path: str, cluster: int, bpb: dict) -> int:
    """Read a 32-bit FAT entry (masked to 28 bits)."""
    fat_offset = bpb["fat1_lba"] * 512 + cluster * 4
    with open(disk_path, "rb") as f:
        f.seek(fat_offset)
        val = struct.unpack("<I", f.read(4))[0]
    return val & 0x0FFFFFFF


def write_fat_entry(disk_path: str, cluster: int, value: int, bpb: dict,
                    fat_num: int = 1) -> None:
    """Write a 32-bit FAT entry to FAT1 or FAT2."""
    fat_lba = bpb["fat1_lba"] if fat_num == 1 else bpb["fat2_lba"]
    fat_offset = fat_lba * 512 + cluster * 4
    with open(disk_path, "r+b") as f:
        f.seek(fat_offset)
        existing = struct.unpack("<I", f.read(4))[0]
        # Preserve the top 4 bits
        new_val = (existing & 0xF0000000) | (value & 0x0FFFFFFF)
        f.seek(fat_offset)
        f.write(struct.pack("<I", new_val))


def find_free_cluster(disk_path: str, bpb: dict, start: int = 3) -> int:
    """Find the first free cluster at or after `start`."""
    total = (bpb["total_sectors_32"] - bpb["data_lba"]) // bpb["sectors_per_cluster"]
    for c in range(start, total + 2):
        if read_fat_entry(disk_path, c, bpb) == 0:
            return c
    raise RuntimeError("Disk full — no free cluster found")


def read_sector(disk_path: str, lba: int) -> bytes:
    with open(disk_path, "rb") as f:
        f.seek(lba * 512)
        return f.read(512)


def write_sector(disk_path: str, lba: int, data: bytes) -> None:
    assert len(data) == 512
    with open(disk_path, "r+b") as f:
        f.seek(lba * 512)
        f.write(data)


# ── Disk image creation ────────────────────────────────────────────────────────

def create_disk(path: str) -> None:
    """Create a fresh 64 MB FAT32 disk image."""
    with open(path, "wb") as f:
        f.seek(DISK_SIZE_MB * 1024 * 1024 - 1)
        f.write(b'\x00')
    subprocess.run(
        ["mkfs.fat", "-F", "32", "-n", "CHIMERA", path],
        check=True, capture_output=True
    )


def write_file_to_disk(disk_path: str, filename: str, content: bytes) -> None:
    """Write a file to the FAT32 disk image using mcopy."""
    with tempfile.NamedTemporaryFile(delete=False, suffix=".tmp") as tf:
        tf.write(content)
        tf_path = tf.name
    try:
        env = os.environ.copy()
        env["MTOOLSRC"] = "/dev/null"
        subprocess.run(
            ["mcopy", "-i", disk_path, tf_path, f"::{filename}"],
            check=True, capture_output=True, env=env
        )
    finally:
        os.unlink(tf_path)


def write_corrupt_type(disk_path: str, corrupt_type: str) -> None:
    """Write a 1-byte /CORRUPT_TYPE file to the disk."""
    write_file_to_disk(disk_path, "CORRUPT_TYPE", corrupt_type.encode())


# ── Corruption injectors ───────────────────────────────────────────────────────

def inject_c1_fat_mismatch(disk_path: str) -> None:
    """C1: Corrupt FAT2 sector 0 (flip a few bytes)."""
    bpb = parse_bpb(disk_path)
    fat2_sector = read_sector(disk_path, bpb["fat2_lba"])
    corrupted = bytearray(fat2_sector)
    # Flip bytes 8-11 (cluster 2 entry in FAT2)
    corrupted[8]  ^= 0xFF
    corrupted[9]  ^= 0xFF
    corrupted[10] ^= 0xFF
    corrupted[11] ^= 0xFF
    write_sector(disk_path, bpb["fat2_lba"], bytes(corrupted))


def inject_c2_orphaned_cluster(disk_path: str) -> None:
    """C2: Allocate a cluster chain in FAT1 and FAT2 but create no dir entry."""
    bpb = parse_bpb(disk_path)
    # Find two free clusters
    c1 = find_free_cluster(disk_path, bpb, start=4)
    c2 = find_free_cluster(disk_path, bpb, start=c1 + 1)
    # Write a chain: c1 → c2 → EOC
    EOC = 0x0FFFFFFF
    write_fat_entry(disk_path, c1, c2, bpb, fat_num=1)
    write_fat_entry(disk_path, c1, c2, bpb, fat_num=2)
    write_fat_entry(disk_path, c2, EOC, bpb, fat_num=1)
    write_fat_entry(disk_path, c2, EOC, bpb, fat_num=2)
    # Write some data to the cluster so it's not empty
    lba = cluster_to_lba(c1, bpb)
    write_sector(disk_path, lba, b"ORPHAN DATA" + b"\x00" * 501)


def inject_c3_cross_link(disk_path: str) -> None:
    """C3: Make file_b.txt point to the same cluster as file_a.txt."""
    bpb = parse_bpb(disk_path)
    # Find file_a.txt's cluster by scanning the root directory
    root_lba = cluster_to_lba(bpb["root_cluster"], bpb)
    sector   = bytearray(read_sector(disk_path, root_lba))

    file_a_cluster = None
    file_b_entry_off = None

    for i in range(0, 512, 32):
        if sector[i] == 0x00:
            break
        if sector[i] == 0xE5:
            continue
        attr = sector[i + 11]
        if attr == 0x0F:
            continue
        name = sector[i:i+8].rstrip(b' ')
        ext  = sector[i+8:i+11].rstrip(b' ')
        if name == b"FILE_A" and ext == b"TXT":
            hi = struct.unpack_from("<H", sector, i + 20)[0]
            lo = struct.unpack_from("<H", sector, i + 26)[0]
            file_a_cluster = (hi << 16) | lo
        if name == b"FILE_B" and ext == b"TXT":
            file_b_entry_off = i

    if file_a_cluster is None or file_b_entry_off is None:
        raise RuntimeError("C3: Could not find file_a.txt or file_b.txt in root dir")

    # Overwrite file_b.txt's cluster pointer to point to file_a's cluster
    struct.pack_into("<H", sector, file_b_entry_off + 20,
                     (file_a_cluster >> 16) & 0xFFFF)
    struct.pack_into("<H", sector, file_b_entry_off + 26,
                     file_a_cluster & 0xFFFF)
    write_sector(disk_path, root_lba, bytes(sector))


def inject_c4_invalid_entry(disk_path: str) -> None:
    """C4: Write a directory entry with attr=0xFF into the root directory."""
    bpb = parse_bpb(disk_path)
    root_lba = cluster_to_lba(bpb["root_cluster"], bpb)
    sector   = bytearray(read_sector(disk_path, root_lba))

    # Find the first free slot (0x00) after existing entries
    insert_off = None
    for i in range(0, 512, 32):
        if sector[i] == 0x00:
            insert_off = i
            break

    if insert_off is None:
        raise RuntimeError("C4: No free slot in root directory")

    # Write a garbage entry with attr=0xFF
    bad_entry = bytearray(32)
    bad_entry[0:8]  = b"BADENTRY"
    bad_entry[8:11] = b"BAD"
    bad_entry[11]   = 0xFF  # invalid attribute
    bad_entry[28:32] = struct.pack("<I", 512)
    sector[insert_off:insert_off + 32] = bad_entry
    # Make sure the next slot is still 0x00 (end of dir)
    if insert_off + 32 < 512:
        sector[insert_off + 32] = 0x00
    write_sector(disk_path, root_lba, bytes(sector))


def inject_c5_orphaned_lfn(disk_path: str) -> None:
    """C5: Write an LFN entry sequence without a following 8.3 entry."""
    bpb = parse_bpb(disk_path)
    root_lba = cluster_to_lba(bpb["root_cluster"], bpb)
    sector   = bytearray(read_sector(disk_path, root_lba))

    # Find the first free slot
    insert_off = None
    for i in range(0, 512, 32):
        if sector[i] == 0x00:
            insert_off = i
            break

    if insert_off is None or insert_off + 64 > 512:
        raise RuntimeError("C5: Not enough space for orphaned LFN")

    # Write an LFN LAST entry (seq=1|0x40=0x41) with no following 8.3
    lfn_entry = bytearray(32)
    lfn_entry[0]  = 0x41   # LAST | seq=1
    lfn_entry[11] = 0x0F   # LFN attr
    lfn_entry[12] = 0x00   # type
    lfn_entry[13] = 0xAB   # checksum (arbitrary — won't match any 8.3)
    # name1: "orphan" in UTF-16LE
    name_utf16 = "orphan".encode("utf-16-le")
    lfn_entry[1:11] = name_utf16[:10]
    lfn_entry[14:26] = name_utf16[10:22] if len(name_utf16) > 10 else b'\xff' * 12
    lfn_entry[26:28] = b'\x00\x00'  # fst_clus_lo must be 0
    lfn_entry[28:32] = b'\xff\xff\xff\xff'  # name3 padding

    sector[insert_off:insert_off + 32] = lfn_entry
    # Leave the next slot as 0x00 (no following 8.3)
    sector[insert_off + 32] = 0x00
    write_sector(disk_path, root_lba, bytes(sector))


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
        serial_file = tempfile.NamedTemporaryFile(
            suffix=".serial", delete=False, mode="wb"
        )
        serial_file.close()
        self._serial_path = serial_file.name
        self.serial_lines = []
        self._stop_reader = False

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
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        self._reader_thread = threading.Thread(
            target=self._read_serial, daemon=True
        )
        self._reader_thread.start()

    def _read_serial(self):
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
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self._lock:
                for line in self.serial_lines:
                    if marker in line:
                        return True
            time.sleep(0.02)
        return False

    def stop(self):
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


# ── fsck.fat verification ──────────────────────────────────────────────────────

def run_fsck(path: str) -> Tuple[bool, str]:
    """Run fsck.fat -n on the image. Returns (clean, output)."""
    result = subprocess.run(
        ["fsck.fat", "-n", path],
        capture_output=True, text=True
    )
    output = result.stdout + result.stderr
    clean = True
    filtered = []
    for line in output.splitlines():
        if not line.strip():
            continue
        if any(skip in line for skip in [
            "fsck.fat", "Warning: Filesystem is FAT32",
            "less than the required minimum", "This may lead to problems",
            "Free cluster summary wrong", "Auto-correcting",
            "Leaving filesystem unchanged", "files, ",
        ]):
            continue
        filtered.append(line)
        clean = False
    return clean, "\n".join(filtered) if not clean else "Clean"


# ── Test scenario runner ───────────────────────────────────────────────────────

SCENARIOS = [
    {
        "id": "C0",
        "name": "Clean disk (baseline)",
        "corrupt_type": "0",
        "inject": None,
        "setup_files": ["testfile.txt"],
    },
    {
        "id": "C1",
        "name": "FAT table mismatch",
        "corrupt_type": "1",
        "inject": inject_c1_fat_mismatch,
        "setup_files": ["testfile.txt"],
    },
    {
        "id": "C2",
        "name": "Orphaned cluster chain",
        "corrupt_type": "2",
        "inject": inject_c2_orphaned_cluster,
        "setup_files": [],
    },
    {
        "id": "C3",
        "name": "Cross-linked files",
        "corrupt_type": "3",
        "inject": inject_c3_cross_link,
        "setup_files": ["file_a.txt", "file_b.txt"],
    },
    {
        "id": "C4",
        "name": "Invalid directory entry (attr=0xFF)",
        "corrupt_type": "4",
        "inject": inject_c4_invalid_entry,
        "setup_files": ["goodfile.txt"],
    },
    {
        "id": "C5",
        "name": "Orphaned LFN entries",
        "corrupt_type": "5",
        "inject": inject_c5_orphaned_lfn,
        "setup_files": ["normal.txt"],
    },
]


def run_scenario(scenario: dict, iso: str) -> dict:
    sid   = scenario["id"]
    sname = scenario["name"]
    print(f"\n{'='*60}")
    print(f"Scenario {sid}: {sname}")
    print('='*60)

    disk_fd, disk_path = tempfile.mkstemp(suffix=".img", prefix=f"fsck_{sid}_")
    os.close(disk_fd)

    result = {
        "id": sid,
        "name": sname,
        "kernel_tests_passed": 0,
        "kernel_tests_failed": 0,
        "kernel_done": False,
        "fsck_clean": False,
        "fsck_output": "",
        "serial_log": [],
        "overall_pass": False,
    }

    try:
        # 1. Create fresh disk
        print(f"  Creating fresh FAT32 disk: {disk_path}")
        create_disk(disk_path)

        # 2. Write setup files
        for fname in scenario.get("setup_files", []):
            content = f"Content of {fname}\n".encode()
            write_file_to_disk(disk_path, fname, content)
            print(f"  Wrote /{fname}")

        # 3. Write corruption type marker
        write_corrupt_type(disk_path, scenario["corrupt_type"])

        # 4. Inject corruption
        if scenario["inject"] is not None:
            print(f"  Injecting corruption {sid}...")
            try:
                scenario["inject"](disk_path)
                print(f"  Corruption injected")
            except Exception as e:
                print(f"  WARNING: Injection failed: {e}")

        # 4b. Flush writes and verify injection
        os.sync()  # ensure all writes are visible to QEMU
        if scenario["inject"] is not None:
            bpb_v = parse_bpb(disk_path)
            fat4 = read_fat_entry(disk_path, 4, bpb_v)
            fat5 = read_fat_entry(disk_path, 5, bpb_v)
            print(f"  [verify] FAT[4]=0x{fat4:08x} FAT[5]=0x{fat5:08x}")

        # 5. Boot QEMU
        print(f"  Booting QEMU...")
        runner = QEMURunner(iso, disk_path)
        runner.start()

        try:
            done = runner.wait_for_marker("[FSCKTEST] DONE", BOOT_TIMEOUT)
            result["kernel_done"] = done

            log = runner.get_serial_log()
            result["serial_log"] = log

            for line in log:
                if "[FSCKTEST] PASS" in line:
                    result["kernel_tests_passed"] += 1
                    parts = line.split("PASS", 1)[1].strip().split(" ", 1)
                    tid = parts[0] if parts else "?"
                    desc = parts[1] if len(parts) > 1 else ""
                    print(f"  ✅ {tid}: {desc}")
                elif "[FSCKTEST] FAIL" in line:
                    result["kernel_tests_failed"] += 1
                    parts = line.split("FAIL", 1)[1].strip().split(" ", 1)
                    tid = parts[0] if parts else "?"
                    desc = parts[1] if len(parts) > 1 else ""
                    print(f"  ❌ {tid}: {desc}")
                elif "[FSCK]" in line:
                    print(f"  {line}")

            if not done:
                print(f"  ⚠️  TIMEOUT after {BOOT_TIMEOUT}s")
        finally:
            runner.stop()

        # 6. Run fsck.fat on the (now repaired) disk
        print(f"  Running fsck.fat on repaired disk...")
        clean, fsck_out = run_fsck(disk_path)
        result["fsck_clean"] = clean
        result["fsck_output"] = fsck_out
        if clean:
            print(f"  ✅ fsck.fat: clean")
        else:
            print(f"  ❌ fsck.fat: {fsck_out}")

        result["overall_pass"] = (
            result["kernel_done"] and
            result["kernel_tests_failed"] == 0 and
            result["kernel_tests_passed"] > 0 and
            result["fsck_clean"]
        )

    except Exception as e:
        print(f"  ERROR: {e}")
        result["error"] = str(e)
    finally:
        try:
            os.unlink(disk_path)
        except Exception:
            pass

    status = "✅ PASS" if result["overall_pass"] else "❌ FAIL"
    print(f"  Result: {status}")
    return result


# ── Main ───────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="ChimeraFS FSCK test harness")
    parser.add_argument("--iso", type=str, default=str(DEFAULT_ISO),
                        help="Path to chimaera_fsck_test.iso")
    parser.add_argument("--results", type=str, default=str(DEFAULT_RESULTS),
                        help="Path to output JSON results file")
    parser.add_argument("--scenario", type=str, default=None,
                        help="Run only this scenario (e.g. C1)")
    args = parser.parse_args()

    if not os.path.exists(args.iso):
        print(f"ERROR: ISO not found at {args.iso}")
        print("Please run 'make fsck-test-iso' first.")
        sys.exit(1)

    # Check mtools is available for disk setup
    if subprocess.run(["which", "mcopy"], capture_output=True).returncode != 0:
        print("ERROR: mcopy not found. Install mtools: sudo apt-get install mtools")
        sys.exit(1)

    scenarios = SCENARIOS
    if args.scenario:
        scenarios = [s for s in SCENARIOS if s["id"] == args.scenario]
        if not scenarios:
            print(f"ERROR: Unknown scenario '{args.scenario}'")
            sys.exit(1)

    print(f"ChimeraFS FSCK Test Harness")
    print(f"ISO: {args.iso}")
    print(f"Running {len(scenarios)} scenario(s)...")

    all_results = []
    for scenario in scenarios:
        r = run_scenario(scenario, args.iso)
        all_results.append(r)

    # Summary
    passed = sum(1 for r in all_results if r["overall_pass"])
    failed = sum(1 for r in all_results if not r["overall_pass"])

    print(f"\n{'='*60}")
    print(f"SUMMARY: {passed}/{len(all_results)} scenarios passed")
    for r in all_results:
        status = "✅" if r["overall_pass"] else "❌"
        print(f"  {status} {r['id']}: {r['name']}")
    print('='*60)

    # Write results
    summary = {
        "total": len(all_results),
        "passed": passed,
        "failed": failed,
        "overall_pass": (failed == 0),
        "scenarios": all_results,
    }
    os.makedirs(os.path.dirname(args.results), exist_ok=True)
    with open(args.results, "w") as f:
        json.dump(summary, f, indent=2)
    print(f"\nResults written to {args.results}")

    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
