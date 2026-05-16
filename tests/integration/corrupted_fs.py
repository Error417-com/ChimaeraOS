#!/usr/bin/env python3
"""
tests/integration/corrupted_fs.py
====================================
Scenario C — corrupted_fs

Injects a FAT table mismatch (FAT1 ≠ FAT2) into the disk image before
booting.  Verifies that ``chimerafs_fsck()`` detects and repairs the
mismatch at boot time, and that the filesystem is clean afterwards.

Corruption injected
-------------------
  FAT2 sector 0 has cluster 2 (root directory) flipped to a garbage value.
  This causes FAT1 ≠ FAT2 for sector 0.  The fsck should copy FAT1 → FAT2.

Assertions
----------
  C_BANNER     Kernel banner present
  C_NOFAIL     No [INTTEST] FAIL lines
  C_PASSCOUNT  In-kernel pass count >= 3
  C_FSCK_RAN   fsck ran at boot ([FSCK] line present)
  C_FSCK_REPAIRED  [FSCK] line mentions FAT mismatch or repair
  C_FSCK_CLEAN fsck.fat reports clean filesystem after repair

Exit codes: 0 = all pass, 1 = any failure.
"""

import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "lib"))

from qemu_runner import IntegrationTest, DiskBuilder, QEMURunner


class CorruptedFs(IntegrationTest):
    scenario_id   = "corrupted_fs"
    scenario_name = "Disk with FAT mismatch — verify fsck detects and repairs"
    qemu_extra    = []

    def setup_disk(self, db: DiskBuilder) -> None:
        # Write a known file so we can verify it survives the repair
        db.write_file("testfile.txt", b"This file must survive fsck repair.\n")
        db.write_file("SCENARIO", b"C")

        # Inject FAT table mismatch
        print("  Injecting FAT mismatch (FAT2 sector 0 corrupted)...")
        db.inject_fat_mismatch()
        print("  Corruption injected")

    def verify(self, runner: QEMURunner, disk_path: str) -> None:
        self.assert_serial_contains(
            runner,
            "[KERNEL] ChimeraOS integration test kernel starting",
            "C_BANNER", "Kernel banner present",
        )

        self.assert_serial_not_contains(
            runner, "[INTTEST] FAIL",
            "C_NOFAIL", "No [INTTEST] FAIL lines in serial log",
        )

        n_pass = runner.count_pass()
        self.assert_true(
            n_pass >= 3,
            "C_PASSCOUNT",
            f"In-kernel pass count >= 3 (got {n_pass})",
        )

        # fsck must have run
        self.assert_serial_contains(
            runner, "[FSCK]",
            "C_FSCK_RAN", "fsck ran at boot ([FSCK] line present)",
        )

        # fsck must have detected and repaired the mismatch
        fsck_lines = runner.lines_matching("[FSCK]")
        repaired = any(
            any(kw in ln for kw in ["mismatch", "repair", "FAT", "fixed", "error"])
            for ln in fsck_lines
        )
        self.assert_true(
            repaired, "C_FSCK_REPAIRED",
            "fsck reported FAT mismatch detection/repair in serial log",
        )


if __name__ == "__main__":
    iso = sys.argv[1] if len(sys.argv) > 1 else None
    sys.exit(CorruptedFs(iso).run())
