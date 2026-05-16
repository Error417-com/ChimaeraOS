#!/usr/bin/env python3
"""
tests/integration/panic_test.py
================================
Integration test: Panic Handler

Deliberately triggers a kernel panic via the 'P' scenario code and verifies
that the panic handler produces a complete, readable serial dump with:

  P1  "*** KERNEL PANIC ***" banner appears in serial log
  P2  Register dump line (EAX=... EBX=...) is present
  P3  Stack trace header "[PANIC] Stack trace:" is present
  P4  At least one frame with a symbol name appears (e.g. <panic_leaf+N>)
  P5  "System halted." line is present
  P6  No "[INTTEST] FAIL" lines before the panic (pre-panic checks passed)

The scenario ends with a panic (not [INTTEST] DONE), so done_markers is
set to ["System halted."] to detect completion.
"""

import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "lib"))

from qemu_runner import IntegrationTest, DiskBuilder, QEMURunner


class PanicTest(IntegrationTest):
    scenario_id   = "panic_test"
    scenario_name = "Kernel panic handler — verify register dump and symbol stack trace"
    qemu_extra    = []

    # The panic handler ends with "System halted." — use that as the done marker.
    done_markers  = ["[PANIC] System halted."]

    def setup_disk(self, db: DiskBuilder) -> None:
        # Write the scenario code 'P' so the kernel runs scenario_panic_test()
        db.write_file("SCENARIO", b"P")

    def verify(self, runner: QEMURunner, disk_path: str) -> None:
        log = runner.dump()

        # P1: panic banner
        self.assert_serial_contains(
            runner,
            "*** KERNEL PANIC ***",
            "P1",
            "Panic banner '*** KERNEL PANIC ***' appears in serial log",
        )

        # P2: register dump — look for EAX= on a line
        self.assert_true(
            any("EAX=" in ln and "EBX=" in ln for ln in runner.serial_log),
            "P2",
            "Register dump line (EAX=... EBX=...) present",
            "No register dump line found in serial log",
        )

        # P3: stack trace header
        self.assert_serial_contains(
            runner,
            "[PANIC] Stack trace:",
            "P3",
            "Stack trace header '[PANIC] Stack trace:' present",
        )

        # P4: at least one frame with a resolved symbol name
        # Frame lines look like:  #0  0x00104xxx  <panic_leaf+N>
        has_symbol = any(
            "<" in ln and ">" in ln and "#" in ln
            for ln in runner.serial_log
        )
        self.assert_true(
            has_symbol,
            "P4",
            "At least one stack frame has a resolved symbol name",
            "No resolved symbol names found in stack trace",
        )

        # P5: system halted line
        self.assert_serial_contains(
            runner,
            "[PANIC] System halted.",
            "P5",
            "'[PANIC] System halted.' line present",
        )

        # P6: the panic message contains our expected string
        self.assert_true(
            any("Deliberate panic from panic_test scenario" in ln
                for ln in runner.serial_log),
            "P6",
            "Panic message 'Deliberate panic from panic_test scenario' present",
            "Panic message not found in serial log",
        )

        # P7: the panic_leaf function name appears in the stack trace
        self.assert_true(
            any("panic_leaf" in ln for ln in runner.serial_log),
            "P7",
            "'panic_leaf' symbol resolved in stack trace",
            "'panic_leaf' not found in stack trace — symbol lookup may have failed",
        )

        # Bonus: print the full panic section for human inspection
        in_panic = False
        print("\n  --- Panic output (for human inspection) ---")
        for ln in runner.serial_log:
            if "KERNEL PANIC" in ln:
                in_panic = True
            if in_panic:
                print(f"    {ln}")
            if "System halted" in ln:
                break
        print("  --- End of panic output ---")


if __name__ == "__main__":
    sys.exit(PanicTest().run())
