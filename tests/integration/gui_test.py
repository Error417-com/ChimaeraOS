"""
ChimaeraOS — GUI Compositor Demo Integration Test
tests/integration/gui_test.py

Boots chimaera_gui_demo.iso under QEMU and verifies the serial log.

Assertions:
  G1  [GUI_DEMO] starting appears
  G2  Shell window created (idx=0)
  G3  Browser window created (idx=1)
  G4  Scheduler starts
  G5  Both tasks exit cleanly
  G6  [GUI_DEMO] PASS reported
  G7  No kernel panic
"""

import sys
import os
import subprocess
import time
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "lib"))
from qemu_runner import IntegrationTest, QEMURunner, DiskBuilder  # type: ignore

GUI_ISO = os.path.join(
    os.path.dirname(__file__), "..", "..", "chimaera_gui_demo.iso"
)


class GuiTest(IntegrationTest):
    scenario_id   = "gui_test"
    scenario_name = "GUI compositor — two-window demo (Shell + Browser)"
    done_markers  = ["[GUI_DEMO] PASS", "[GUI_DEMO] FAIL", "System halted."]

    def setup_disk(self, db: DiskBuilder) -> None:
        # GUI demo kernel does not use the disk; provide a minimal marker file.
        db.write_file("SCENARIO", b"G")

    def verify(self, runner: QEMURunner, disk_path: str) -> None:
        # G1: Demo starts
        self.assert_serial_contains(
            runner,
            "[GUI_DEMO] starting",
            "G1",
            "[GUI_DEMO] starting appears",
        )
        # G2: Shell window created
        self.assert_serial_contains(
            runner,
            "window created: Shell (idx=0)",
            "G2",
            "Shell window created at idx=0",
        )
        # G3: Browser window created
        self.assert_serial_contains(
            runner,
            "window created: Browser (idx=1)",
            "G3",
            "Browser window created at idx=1",
        )
        # G4: Scheduler starts
        self.assert_serial_contains(
            runner,
            "[SCHED] Starting scheduler",
            "G4",
            "Scheduler starts",
        )
        # G5: Both tasks exit cleanly
        self.assert_serial_contains(
            runner,
            "[SCHED] All tasks complete",
            "G5",
            "Both tasks exit cleanly",
        )
        # G6: Demo reports PASS
        self.assert_serial_contains(
            runner,
            "[GUI_DEMO] PASS",
            "G6",
            "[GUI_DEMO] PASS reported",
        )
        # G7: No kernel panic
        log = "\n".join(runner.serial_log)
        self.assert_true(
            "*** KERNEL PANIC ***" not in log,
            "G7",
            "No kernel panic",
            "Kernel panic detected in serial log",
        )


class GuiTestRunner(GuiTest):
    """Subclass that overrides run() to boot the GUI demo ISO directly."""

    def run(self):
        iso_path = os.path.abspath(GUI_ISO)
        if not os.path.exists(iso_path):
            print(f"[SKIP] GUI demo ISO not found: {iso_path}")
            print("       Run 'make gui-demo-iso' first.")
            sys.exit(0)

        # Build a temporary (unused) disk image
        with tempfile.NamedTemporaryFile(suffix=".img", delete=False) as f:
            disk_path = f.name
        try:
            db = DiskBuilder(disk_path)
            self.setup_disk(db)

            mon_sock       = tempfile.mktemp(suffix=".sock")
            serial_log_file = tempfile.mktemp(suffix=".log")

            qemu_cmd = [
                "qemu-system-i386",
                "-cdrom",   iso_path,
                "-drive",   f"file={disk_path},format=raw,if=ide,index=1",
                "-boot",    "d",
                "-serial",  f"file:{serial_log_file}",
                "-display", "none",
                "-m",       "128",
                "-monitor", f"unix:{mon_sock},server,nowait",
                "-no-reboot",
            ]

            proc = subprocess.Popen(
                qemu_cmd,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

            # Wait for PASS or timeout (30 seconds)
            TIMEOUT = 30
            start   = time.time()
            log_text = ""
            try:
                while time.time() - start < TIMEOUT:
                    time.sleep(0.5)
                    if os.path.exists(serial_log_file):
                        with open(serial_log_file) as f:
                            log_text = f.read()
                        if any(m in log_text for m in self.done_markers):
                            break
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except Exception:
                    proc.kill()

            # Build a minimal runner-like object for verify()
            class _FakeRunner:
                def __init__(self, text):
                    self._text = text
                    self.serial_log = text.splitlines()
                def contains(self, s):
                    return s in self._text
                def not_contains(self, s):
                    return s not in self._text

            runner = _FakeRunner(log_text)

            print(f"\n{'='*70}")
            print(f"Scenario: {self.scenario_id}")
            print(f"  {self.scenario_name}")
            print(f"{'='*70}")

            self.verify(runner, disk_path)

            passed = sum(1 for a in self._assertions if a["ok"])
            total  = len(self._assertions)
            print(f"\n{'━'*70}")
            print(f"  PASS: {passed}/{total} assertions passed")
            print(f"{'━'*70}")

            if passed == total:
                print(f"  ✓ PASS  {self.scenario_id}")
            else:
                print(f"  ✗ FAIL  {self.scenario_id}")

            sys.exit(0 if passed == total else 1)

        finally:
            if os.path.exists(disk_path):
                os.unlink(disk_path)
            if os.path.exists(serial_log_file):
                os.unlink(serial_log_file)


if __name__ == "__main__":
    GuiTestRunner().run()
