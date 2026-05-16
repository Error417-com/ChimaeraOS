"""
tests/integration/lib/qemu_runner.py
========================================
Shared library for ChimeraOS integration tests.

Provides:
  DiskBuilder   — create and manipulate FAT32 disk images
  QEMURunner    — boot a kernel ISO with a data disk, capture serial output
  IntegrationTest — base class for scenario scripts

Usage
-----
  from qemu_runner import IntegrationTest, DiskBuilder, QEMURunner

  class MyScenario(IntegrationTest):
      scenario_id   = "my_scenario"
      scenario_name = "Description"
      qemu_extra    = []          # extra QEMU flags

      def setup_disk(self, db: DiskBuilder) -> None:
          db.write_file("SCENARIO", b"X")

      def verify(self, runner: QEMURunner, disk_path: str) -> None:
          self.assert_serial_contains(runner, "expected text", "ID", "desc")

  if __name__ == "__main__":
      import sys
      sys.exit(MyScenario().run())
"""

from __future__ import annotations

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import List, Optional

# ── Paths ─────────────────────────────────────────────────────────────────────

REPO_ROOT = Path(__file__).resolve().parents[3]
ISO_PATH  = REPO_ROOT / "chimaera_integration_test.iso"

# ── DiskBuilder ───────────────────────────────────────────────────────────────

class BPB:
    """Parse the BIOS Parameter Block from a FAT32 image."""

    def __init__(self, data: bytes):
        (
            self.bytes_per_sector,
            self.sectors_per_cluster,
            self.reserved_sectors,
            self.num_fats,
            _root_entry_count,
            _total_sectors_16,
            _media,
            _fat_size_16,
            _sectors_per_track,
            _num_heads,
            _hidden_sectors,
            self.total_sectors_32,
        ) = struct.unpack_from("<HBHBHHBHHHI I", data, 11)

        (self.fat_size_32,) = struct.unpack_from("<I", data, 36)
        (self.root_cluster,) = struct.unpack_from("<I", data, 44)

        self.fat1_lba  = self.reserved_sectors
        self.fat2_lba  = self.fat1_lba + self.fat_size_32
        self.data_lba  = self.fat2_lba + self.fat_size_32
        self.cluster_size = self.bytes_per_sector * self.sectors_per_cluster
        self.total_clusters = (
            (self.total_sectors_32 - self.data_lba) // self.sectors_per_cluster
        )

    def fat_offset(self, cluster: int, fat_num: int = 1) -> int:
        lba = self.fat1_lba if fat_num == 1 else self.fat2_lba
        return lba * self.bytes_per_sector + cluster * 4

    def cluster_offset(self, cluster: int) -> int:
        return (self.data_lba + (cluster - 2) * self.sectors_per_cluster) * self.bytes_per_sector


class DiskBuilder:
    """Create and manipulate a FAT32 disk image for integration tests."""

    # Default disk size: 64 MiB — needed to satisfy FAT32 minimum cluster count
    # for mtools to work correctly (MTOOLSRC=/dev/null also required)
    DEFAULT_SIZE_MB = 64

    def __init__(self, path: str, size_mb: int = DEFAULT_SIZE_MB):
        self.path     = path
        self.size_mb  = size_mb
        self._bpb: Optional[BPB] = None
        self._create()

    # ── Creation ──────────────────────────────────────────────────────────────

    def _create(self) -> None:
        """Create a fresh FAT32 image."""
        # Use seek to create a sparse file (faster than dd)
        with open(self.path, 'wb') as f:
            f.seek(self.size_mb * 1024 * 1024 - 1)
            f.write(b'\x00')
        subprocess.run(
            ["mkfs.fat", "-F", "32", "-n", "CHIMAERA", self.path],
            check=True, capture_output=True,
            stdin=subprocess.DEVNULL,
        )

    # ── BPB access ────────────────────────────────────────────────────────────

    @property
    def bpb(self) -> BPB:
        if self._bpb is None:
            with open(self.path, "rb") as f:
                self._bpb = BPB(f.read(512))
        return self._bpb

    def _invalidate_bpb(self) -> None:
        self._bpb = None

    # ── File operations ───────────────────────────────────────────────────────

    def _mtools_env(self) -> dict:
        env = os.environ.copy()
        env["MTOOLSRC"] = "/dev/null"
        return env

    def mkdir(self, path: str) -> None:
        """Create a directory (and parents) on the disk image."""
        env = self._mtools_env()
        subprocess.run(
            ["mmd", "-i", self.path, f"::{path}"],
            check=True, capture_output=True, env=env,
            stdin=subprocess.DEVNULL,
        )

    def _dir_exists(self, path: str) -> bool:
        """Return True if a directory exists on the disk image."""
        env = self._mtools_env()
        r = subprocess.run(
            ["mdir", "-i", self.path, f"::{path}"],
            capture_output=True, env=env,
            stdin=subprocess.DEVNULL, timeout=15,
        )
        return r.returncode == 0

    def write_file(self, dest_path: str, data: bytes) -> None:
        """Write bytes to a file on the disk image."""
        with tempfile.NamedTemporaryFile(delete=False) as tmp:
            tmp.write(data)
            tmp_name = tmp.name
        try:
            env = self._mtools_env()
            # Ensure parent directories exist (only create if they don't already)
            parts = dest_path.replace("\\", "/").split("/")
            for i in range(1, len(parts)):
                parent = "/".join(parts[:i])
                if not self._dir_exists(parent):
                    subprocess.run(
                        ["mmd", "-i", self.path, f"::{parent}"],
                        check=True, capture_output=True, env=env,
                        stdin=subprocess.DEVNULL, timeout=15,
                    )
            subprocess.run(
                ["mcopy", "-i", self.path, tmp_name,
                 f"::{dest_path}"],
                check=True, capture_output=True, env=env,
                stdin=subprocess.DEVNULL, timeout=30,
            )
        finally:
            os.unlink(tmp_name)
        self._invalidate_bpb()

    # ── Disk-fill helper ──────────────────────────────────────────────────────

    def fill_to_pct(self, target_pct: float) -> None:
        """Fill the disk to approximately target_pct% full.

        Writes a single large file to avoid thousands of mcopy invocations.
        Uses chunked I/O to avoid large in-memory buffers.
        """
        bpb = self.bpb
        target_used = int(bpb.total_clusters * target_pct / 100)
        used = self._count_used_clusters()
        to_fill = target_used - used
        if to_fill <= 0:
            return
        fill_bytes = to_fill * bpb.cluster_size
        fill_bytes = min(fill_bytes, 512 * 1024 * 1024)

        # Write fill data to a temp file in 64 KiB chunks
        chunk = b"\xAB" * 65536
        with tempfile.NamedTemporaryFile(delete=False) as tmp:
            tmp_name = tmp.name
            remaining = fill_bytes
            while remaining > 0:
                n = min(len(chunk), remaining)
                tmp.write(chunk[:n])
                remaining -= n
        try:
            env = self._mtools_env()
            subprocess.run(
                ["mcopy", "-i", self.path, tmp_name, "::fill_bulk.bin"],
                check=True, capture_output=True, env=env,
                stdin=subprocess.DEVNULL,
            )
        finally:
            os.unlink(tmp_name)
        self._invalidate_bpb()

    def _count_used_clusters(self) -> int:
        """Count non-free clusters in FAT1."""
        bpb = self.bpb
        with open(self.path, "rb") as f:
            f.seek(bpb.fat1_lba * bpb.bytes_per_sector)
            fat_data = f.read(bpb.fat_size_32 * bpb.bytes_per_sector)
        count = 0
        for c in range(2, bpb.total_clusters + 2):
            val = struct.unpack_from("<I", fat_data, c * 4)[0] & 0x0FFFFFFF
            if val != 0:
                count += 1
        return count

    def free_pct(self) -> float:
        """Return the percentage of disk space that is free."""
        bpb = self.bpb
        used = self._count_used_clusters()
        return 100.0 * (bpb.total_clusters - used) / bpb.total_clusters

    # ── Corruption injection ──────────────────────────────────────────────────

    def inject_fat_mismatch(self) -> None:
        """Corrupt FAT2 sector 0 so FAT1 ≠ FAT2."""
        bpb = self.bpb
        fat2_byte_off = bpb.fat2_lba * bpb.bytes_per_sector
        with open(self.path, "r+b") as f:
            # Flip cluster 3 in FAT2 to a garbage value
            off = fat2_byte_off + 3 * 4
            f.seek(off)
            orig = struct.unpack("<I", f.read(4))[0]
            f.seek(off)
            f.write(struct.pack("<I", orig ^ 0xDEAD0000))

    def inject_orphaned_cluster(self) -> None:
        """Allocate a cluster chain in FAT1+FAT2 that no dir entry points to."""
        bpb = self.bpb
        # Find two free clusters
        with open(self.path, "rb") as f:
            f.seek(bpb.fat1_lba * bpb.bytes_per_sector)
            fat_data = f.read(bpb.fat_size_32 * bpb.bytes_per_sector)

        free_clusters = []
        for c in range(2, bpb.total_clusters + 2):
            val = struct.unpack_from("<I", fat_data, c * 4)[0] & 0x0FFFFFFF
            if val == 0:
                free_clusters.append(c)
            if len(free_clusters) == 2:
                break

        if len(free_clusters) < 2:
            raise RuntimeError("Not enough free clusters to inject orphan")

        c1, c2 = free_clusters

        def write_fat_entry(fat_lba: int, cluster: int, value: int) -> None:
            off = fat_lba * bpb.bytes_per_sector + cluster * 4
            with open(self.path, "r+b") as f:
                f.seek(off)
                f.write(struct.pack("<I", value))

        write_fat_entry(bpb.fat1_lba, c1, c2)
        write_fat_entry(bpb.fat1_lba, c2, 0x0FFFFFFF)
        write_fat_entry(bpb.fat2_lba, c1, c2)
        write_fat_entry(bpb.fat2_lba, c2, 0x0FFFFFFF)


# ── QEMURunner ────────────────────────────────────────────────────────────────

class QEMURunner:
    """Boot a ChimeraOS ISO with a data disk and capture serial output."""

    QEMU_BIN    = "qemu-system-i386"
    TIMEOUT_SEC = 90

    def __init__(
        self,
        iso_path: str,
        disk_path: str,
        extra_flags: List[str] = None,
        timeout: int = TIMEOUT_SEC,
        done_markers: List[str] = None,
    ):
        self.iso_path    = iso_path
        self.disk_path   = disk_path
        self.extra_flags = extra_flags or []
        self.timeout     = timeout
        # Markers that indicate the kernel has finished (any one is sufficient).
        # Defaults to [INTTEST] DONE; override for scenarios that end with panic.
        self.done_markers: List[str] = done_markers or ["[INTTEST] DONE"]
        self.serial_log: List[str] = []
        self._serial_path: Optional[str] = None

    def run(self) -> int:
        """Boot QEMU and wait for [INTTEST] DONE.  Returns exit code."""
        with tempfile.NamedTemporaryFile(
            suffix=".log", delete=False, mode="w"
        ) as f:
            self._serial_path = f.name

        os.sync()

        cmd = [
            self.QEMU_BIN,
            "-m", "128",
            "-no-reboot",
            "-display", "none",
            "-serial", f"file:{self._serial_path}",
            "-boot", "d",
            "-cdrom", self.iso_path,
            "-drive",
            f"file={self.disk_path},if=ide,index=1,format=raw,cache=none",
        ] + self.extra_flags

        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        deadline = time.time() + self.timeout
        done = False
        while time.time() < deadline:
            time.sleep(1)
            self._reload_log()
            if any(
                any(marker in ln for ln in self.serial_log)
                for marker in self.done_markers
            ):
                done = True
                break

        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

        self._reload_log()
        return 0 if done else 1

    def _reload_log(self) -> None:
        if self._serial_path and os.path.exists(self._serial_path):
            with open(self._serial_path, "r", errors="replace") as f:
                self.serial_log = f.read().splitlines()

    def contains(self, text: str) -> bool:
        return any(text in ln for ln in self.serial_log)

    def not_contains(self, text: str) -> bool:
        return not self.contains(text)

    def lines_matching(self, prefix: str) -> List[str]:
        return [ln for ln in self.serial_log if prefix in ln]

    def count_pass(self) -> int:
        return sum(1 for ln in self.serial_log if "[INTTEST] PASS" in ln)

    def count_fail(self) -> int:
        return sum(1 for ln in self.serial_log if "[INTTEST] FAIL" in ln)

    def dump(self) -> str:
        return "\n".join(self.serial_log)


# ── IntegrationTest base class ────────────────────────────────────────────────

class IntegrationTest:
    """Base class for integration test scenarios."""

    scenario_id   = "unknown"
    scenario_name = "Unknown scenario"
    qemu_extra: List[str] = []

    def __init__(self, iso_path: Optional[str] = None):
        self.iso_path = iso_path or str(ISO_PATH)
        self._assertions: List[dict] = []

    # ── Assertion helpers ─────────────────────────────────────────────────────

    def assert_serial_contains(
        self, runner: QEMURunner, text: str, aid: str, desc: str
    ) -> None:
        ok = runner.contains(text)
        self._record(ok, aid, desc, f"Expected '{text}' in serial log")

    def assert_serial_not_contains(
        self, runner: QEMURunner, text: str, aid: str, desc: str
    ) -> None:
        ok = runner.not_contains(text)
        self._record(ok, aid, desc, f"Unexpected '{text}' found in serial log")

    def assert_true(
        self, cond: bool, aid: str, desc: str, fail_msg: str = ""
    ) -> None:
        self._record(cond, aid, desc, fail_msg or f"Assertion {aid} failed")

    def _record(self, ok: bool, aid: str, desc: str, fail_msg: str) -> None:
        self._assertions.append({
            "id": aid, "ok": ok,
            "desc": desc, "fail_msg": fail_msg,
        })
        status = "PASS" if ok else "FAIL"
        print(f"  [{status}] {aid}: {desc}")
        if not ok:
            print(f"         ↳ {fail_msg}")

    # ── Disk builder factory ──────────────────────────────────────────────────

    def _make_disk(self, path: str) -> DiskBuilder:
        return DiskBuilder(path)

    # ── Subclass interface ────────────────────────────────────────────────────

    def setup_disk(self, db: DiskBuilder) -> None:
        raise NotImplementedError

    def verify(self, runner: QEMURunner, disk_path: str) -> None:
        raise NotImplementedError

    # ── Main entry point ──────────────────────────────────────────────────────

    def run(self) -> int:
        """Run the scenario.  Returns 0 on success, 1 on failure."""
        print(f"\n{'='*70}")
        print(f"Scenario: {self.scenario_id}")
        print(f"  {self.scenario_name}")
        print(f"{'='*70}")

        if not os.path.exists(self.iso_path):
            print(f"ERROR: ISO not found: {self.iso_path}")
            print("  Run: cd <repo> && make integration-test-iso")
            return 1

        with tempfile.NamedTemporaryFile(
            suffix=".img", delete=False
        ) as f:
            disk_path = f.name

        try:
            # Build disk
            print("  Building disk image...")
            db = self._make_disk(disk_path)
            self.setup_disk(db)
            print(f"  Disk ready: {disk_path} ({os.path.getsize(disk_path)//1024//1024} MiB)")

            # Boot QEMU
            print(f"  Booting QEMU (timeout={QEMURunner.TIMEOUT_SEC}s)...")
            runner = QEMURunner(
                iso_path     = self.iso_path,
                disk_path    = disk_path,
                extra_flags  = self.qemu_extra,
                done_markers = getattr(self, 'done_markers', None),
            )
            rc = runner.run()

            if rc != 0:
                print("  WARNING: QEMU did not reach [INTTEST] DONE within timeout")

            # Run assertions
            print("\n  Assertions:")
            self.verify(runner, disk_path)

            # Summary
            passed = sum(1 for a in self._assertions if a["ok"])
            failed = sum(1 for a in self._assertions if not a["ok"])
            total  = len(self._assertions)
            print(f"\n  Result: {passed}/{total} assertions passed")

            if failed:
                print("\n  Serial log (last 30 lines):")
                for ln in runner.serial_log[-30:]:
                    print(f"    {ln}")
                return 1
            return 0

        finally:
            try:
                os.unlink(disk_path)
            except OSError:
                pass
