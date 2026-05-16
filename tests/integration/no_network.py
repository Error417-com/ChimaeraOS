#!/usr/bin/env python3
"""
tests/integration/no_network.py
==================================
Scenario N — no_network

Boots ChimeraOS with QEMU started with ``-nic none`` (no network device).
ChimeraOS has no network stack, so this should be a complete no-op — the
kernel must boot normally and complete all filesystem operations.

This scenario guards against any future network-related code accidentally
being added that panics or stalls when no NIC is present.

QEMU flags used
---------------
  -nic none   — remove all default network devices

Assertions
----------
  N_BANNER    Kernel banner present
  N_NOFAIL    No [INTTEST] FAIL lines
  N_PASSCOUNT In-kernel pass count >= 3
  N_NOPANIC   No [KERNEL] PANIC or [NET] ERROR in serial log
  N_FSCK_RAN  fsck ran at boot
  N_FSCK_CLEAN fsck reported 0 errors

Exit codes: 0 = all pass, 1 = any failure.
"""

import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "lib"))

from qemu_runner import IntegrationTest, DiskBuilder, QEMURunner


class NoNetwork(IntegrationTest):
    scenario_id   = "no_network"
    scenario_name = "No network device — verify boot proceeds normally"
    # Remove all network devices from QEMU
    qemu_extra    = ["-nic", "none"]

    def setup_disk(self, db: DiskBuilder) -> None:
        db.write_file("SCENARIO", b"N")
        db.write_file("net_baseline.txt", b"pre-boot data\n")

    def verify(self, runner: QEMURunner, disk_path: str) -> None:
        self.assert_serial_contains(
            runner,
            "[KERNEL] ChimeraOS integration test kernel starting",
            "N_BANNER", "Kernel banner present with no network device",
        )

        self.assert_serial_not_contains(
            runner, "[INTTEST] FAIL",
            "N_NOFAIL", "No [INTTEST] FAIL lines in serial log",
        )

        n_pass = runner.count_pass()
        self.assert_true(
            n_pass >= 3,
            "N_PASSCOUNT",
            f"In-kernel pass count >= 3 (got {n_pass})",
        )

        # No panic or network error messages
        self.assert_serial_not_contains(
            runner, "PANIC",
            "N_NOPANIC", "No PANIC in serial log",
        )
        self.assert_serial_not_contains(
            runner, "[NET] ERROR",
            "N_NONET_ERR", "No [NET] ERROR in serial log",
        )

        self.assert_serial_contains(
            runner, "[FSCK]",
            "N_FSCK_RAN", "fsck ran at boot",
        )
        fsck_lines = runner.lines_matching("[FSCK]")
        fsck_clean = any("0 error" in ln for ln in fsck_lines)
        self.assert_true(
            fsck_clean, "N_FSCK_CLEAN",
            "fsck reported 0 errors",
        )


if __name__ == "__main__":
    iso = sys.argv[1] if len(sys.argv) > 1 else None
    sys.exit(NoNetwork(iso).run())
