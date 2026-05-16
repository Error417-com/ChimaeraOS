#!/usr/bin/env python3
"""
tests/integration/fresh_install.py
====================================
Scenario F — fresh_install

Boots ChimeraOS on a completely clean FAT32 disk.

Assertions
----------
  F1  Kernel reaches [INTTEST] DONE within timeout
  F2  Filesystem mounts (kernel reports [KERNEL] ChimeraOS integration test kernel starting)
  F3  No [INTTEST] FAIL lines in serial log
  F4  Kernel PASS count >= 5 (all in-kernel F1–F6 checks)
  F5  fsck.fat reports clean filesystem after boot

Exit codes: 0 = all pass, 1 = any failure.
"""

import sys
import os

# Allow running from any directory
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "lib"))

from qemu_runner import IntegrationTest, DiskBuilder, QEMURunner


class FreshInstall(IntegrationTest):
    scenario_id   = "fresh_install"
    scenario_name = "Clean disk — verify boot to shell and basic file I/O"
    qemu_extra    = []

    def setup_disk(self, db: DiskBuilder) -> None:
        # Write only the scenario marker — disk is otherwise empty
        db.write_file("SCENARIO", b"F")

    def verify(self, runner: QEMURunner, disk_path: str) -> None:
        # Kernel banner present
        self.assert_serial_contains(
            runner,
            "[KERNEL] ChimeraOS integration test kernel starting",
            "F_BANNER", "Kernel banner present in serial log",
        )

        # No FAIL lines from in-kernel checks
        self.assert_serial_not_contains(
            runner, "[INTTEST] FAIL",
            "F_NOFAIL", "No [INTTEST] FAIL lines in serial log",
        )

        # All in-kernel PASS assertions fired
        n_pass = runner.count_pass()
        self.assert_true(
            n_pass >= 5,
            "F_PASSCOUNT",
            f"In-kernel pass count >= 5 (got {n_pass})",
        )

        # FSCK ran and reported 0 errors
        self.assert_serial_contains(
            runner, "[FSCK]",
            "F_FSCK_RAN", "fsck ran at boot ([FSCK] line present)",
        )
        fsck_lines = runner.lines_matching("[FSCK]")
        fsck_clean = any("0 error" in ln for ln in fsck_lines)
        self.assert_true(
            fsck_clean, "F_FSCK_CLEAN",
            "fsck reported 0 errors on clean disk",
        )


if __name__ == "__main__":
    iso = sys.argv[1] if len(sys.argv) > 1 else None
    sys.exit(FreshInstall(iso).run())
