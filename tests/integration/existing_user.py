#!/usr/bin/env python3
"""
tests/integration/existing_user.py
=====================================
Scenario U — existing_user

Boots ChimeraOS on a disk that already contains user files:
  /user/prefs.cfg         — user preferences (starts with "theme=")
  /user/history.log       — command history
  /user/data/model.bin    — a nested binary file

Verifies that prior user state survives a reboot intact and that the
kernel can append to existing files.

Assertions
----------
  U_BANNER   Kernel banner present
  U_NOFAIL   No [INTTEST] FAIL lines
  U_PASSCOUNT In-kernel pass count >= 5
  U_FSCK_RAN  fsck ran at boot
  U_FSCK_CLEAN fsck reported 0 errors

Exit codes: 0 = all pass, 1 = any failure.
"""

import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "lib"))

from qemu_runner import IntegrationTest, DiskBuilder, QEMURunner


class ExistingUser(IntegrationTest):
    scenario_id   = "existing_user"
    scenario_name = "Disk with prior user state — verify login and file integrity"
    qemu_extra    = []

    def setup_disk(self, db: DiskBuilder) -> None:
        # Create directory structure
        db.mkdir("user")
        db.mkdir("user/data")

        # User preferences
        db.write_file("user/prefs.cfg",
                      b"theme=dark\nfont_size=14\nlang=en\n")

        # Command history (simulate 10 prior sessions)
        history = b"".join(
            f"session {i}: ls /user\n".encode() for i in range(10)
        )
        db.write_file("user/history.log", history)

        # Binary model file (256 bytes of pseudo-random data)
        model = bytes((i * 37 + 13) & 0xFF for i in range(256))
        db.write_file("user/data/model.bin", model)

        # Scenario marker
        db.write_file("SCENARIO", b"U")

    def verify(self, runner: QEMURunner, disk_path: str) -> None:
        self.assert_serial_contains(
            runner,
            "[KERNEL] ChimeraOS integration test kernel starting",
            "U_BANNER", "Kernel banner present",
        )

        self.assert_serial_not_contains(
            runner, "[INTTEST] FAIL",
            "U_NOFAIL", "No [INTTEST] FAIL lines in serial log",
        )

        n_pass = runner.count_pass()
        self.assert_true(
            n_pass >= 5,
            "U_PASSCOUNT",
            f"In-kernel pass count >= 5 (got {n_pass})",
        )

        self.assert_serial_contains(
            runner, "[FSCK]",
            "U_FSCK_RAN", "fsck ran at boot",
        )
        fsck_lines = runner.lines_matching("[FSCK]")
        fsck_clean = any("0 error" in ln for ln in fsck_lines)
        self.assert_true(
            fsck_clean, "U_FSCK_CLEAN",
            "fsck reported 0 errors on user-state disk",
        )


if __name__ == "__main__":
    iso = sys.argv[1] if len(sys.argv) > 1 else None
    sys.exit(ExistingUser(iso).run())
