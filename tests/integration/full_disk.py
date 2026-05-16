#!/usr/bin/env python3
"""
tests/integration/full_disk.py
=================================
Scenario D — full_disk

Boots ChimeraOS on a disk that has been pre-filled to ~95% capacity.
Verifies that the kernel:
  - Mounts the filesystem successfully
  - Can still write a small file (a few clusters remain)
  - Returns FAT32_ERR_FULL gracefully when the disk is exhausted
  - Does NOT crash or corrupt the filesystem

Assertions
----------
  D_BANNER    Kernel banner present
  D_NOFAIL    No [INTTEST] FAIL lines
  D_PASSCOUNT In-kernel pass count >= 3
  D_FSCK_RAN  fsck ran at boot
  D_FSCK_CLEAN fsck reported 0 errors after full-disk scenario

Exit codes: 0 = all pass, 1 = any failure.
"""

import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "lib"))

from qemu_runner import IntegrationTest, DiskBuilder, QEMURunner


class FullDisk(IntegrationTest):
    scenario_id   = "full_disk"
    scenario_name = "Disk near full — verify graceful FAT32_ERR_FULL handling"
    qemu_extra    = []

    def setup_disk(self, db: DiskBuilder) -> None:
        # Write scenario marker first (before filling)
        db.write_file("SCENARIO", b"D")

        # Fill to 99.9% — leaves ~0.1% (~130 clusters × 512B = ~65 KiB) free.
        # The kernel's small write (3 bytes) uses 1 cluster, leaving ~129 clusters.
        # The large-write loop (64 × 4 KiB = 64 × 8 clusters = 512 clusters) will
        # exhaust the remaining space well within 64 iterations.
        print("  Filling disk to 99.9%...")
        db.fill_to_pct(99.9)
        free = db.free_pct()
        print(f"  Disk free after fill: {free:.2f}%")

    def verify(self, runner: QEMURunner, disk_path: str) -> None:
        self.assert_serial_contains(
            runner,
            "[KERNEL] ChimeraOS integration test kernel starting",
            "D_BANNER", "Kernel banner present on near-full disk",
        )

        self.assert_serial_not_contains(
            runner, "[INTTEST] FAIL",
            "D_NOFAIL", "No [INTTEST] FAIL lines in serial log",
        )

        n_pass = runner.count_pass()
        self.assert_true(
            n_pass >= 3,
            "D_PASSCOUNT",
            f"In-kernel pass count >= 3 (got {n_pass})",
        )

        self.assert_serial_contains(
            runner, "[FSCK]",
            "D_FSCK_RAN", "fsck ran at boot",
        )
        fsck_lines = runner.lines_matching("[FSCK]")
        fsck_clean = any("0 error" in ln for ln in fsck_lines)
        self.assert_true(
            fsck_clean, "D_FSCK_CLEAN",
            "fsck reported 0 errors after full-disk scenario",
        )


if __name__ == "__main__":
    iso = sys.argv[1] if len(sys.argv) > 1 else None
    sys.exit(FullDisk(iso).run())
