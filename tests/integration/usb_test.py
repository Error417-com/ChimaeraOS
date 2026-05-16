"""
ChimaeraOS — USB HID Keyboard Integration Test
tests/integration/usb_test.py

Boots the usb-demo ISO under QEMU with -usb -device usb-kbd, injects
keystrokes via the QEMU monitor, and verifies that the UHCI driver
delivers keypresses to the kernel.

Assertions:
  U1  UHCI controller found on PCI
  U2  At least one HID device enumerated
  U3  Keyboard detected (boot-protocol)
  U4  At least 3 keypress events received
  U5  Correct ASCII characters decoded (h, e, l)
  U6  Demo reports PASS
  U7  No kernel panic
"""

import sys
import os
import subprocess
import time
import threading
import socket

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "lib"))
from qemu_runner import IntegrationTest, QEMURunner, DiskBuilder


USB_ISO = os.path.join(
    os.path.dirname(__file__), "..", "..", "chimaera_usb_demo.iso"
)

# Keys to inject via QEMU monitor (after keyboard is ready)
INJECT_KEYS = ["h", "e", "l", "l", "o"]
# Delay (seconds) before injecting keys — allow full USB enumeration
KEY_INJECT_DELAY = 3.0
# Delay between individual keystrokes
KEY_INTER_DELAY  = 0.4


class UsbTest(IntegrationTest):
    scenario_id   = "usb_test"
    scenario_name = "USB HID keyboard — UHCI enumeration and keypress delivery"
    done_markers  = ["[USB_DEMO] PASS", "[USB_DEMO] FAIL", "System halted."]

    def __init__(self):
        super().__init__(iso_path=os.path.abspath(USB_ISO))
        # Force the ISO path to the USB demo ISO, ignoring sys.argv[1]
        self.iso_path = os.path.abspath(USB_ISO)

    # ── Extra QEMU args ───────────────────────────────────────────────────────

    def extra_qemu_args(self):
        """Return additional QEMU arguments for USB keyboard emulation."""
        return ["-usb", "-device", "usb-kbd"]

    # ── setup_disk ────────────────────────────────────────────────────────────

    def setup_disk(self, db: DiskBuilder) -> None:
        # USB demo kernel does not use the disk; provide a minimal marker file.
        db.write_file("SCENARIO", b"U")

    # ── _inject_keys_thread ───────────────────────────────────────────────────

    @staticmethod
    def _inject_keys_thread(monitor_sock: str, keys: list, delay: float,
                             inter: float):
        """Background thread: wait `delay` seconds then inject keystrokes."""
        time.sleep(delay)
        for key in keys:
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
                    s.connect(monitor_sock)
                    s.recv(256)          # consume the QEMU monitor banner
                    s.sendall(f"sendkey {key}\n".encode())
                    time.sleep(0.05)
            except OSError:
                pass
            time.sleep(inter)

    # ── verify ────────────────────────────────────────────────────────────────

    def verify(self, runner: QEMURunner, disk_path: str) -> None:
        log = "\n".join(runner.serial_log)

        # U1: UHCI controller found
        self.assert_serial_contains(
            runner,
            "[UHCI] Controller at PCI",
            "U1",
            "UHCI controller found on PCI",
        )

        # U2: At least one HID device enumerated
        hid_lines = [l for l in log.splitlines()
                     if "[USB_DEMO] HID devices:" in l]
        hid_count = 0
        if hid_lines:
            try:
                hid_count = int(hid_lines[-1].split(":")[-1].strip())
            except ValueError:
                pass
        self.assert_true(
            hid_count >= 1,
            "U2",
            f"At least 1 HID device enumerated (got {hid_count})",
            f"No HID devices enumerated (got {hid_count})",
        )

        # U3: Keyboard detected
        self.assert_serial_contains(
            runner,
            "[KBD] Boot-protocol keyboard ready",
            "U3",
            "Boot-protocol keyboard detected and ready",
        )

        # U4: At least 3 keypress events received
        key_lines = [l for l in log.splitlines()
                     if "[USB_DEMO] keypress:" in l]
        self.assert_true(
            len(key_lines) >= 3,
            "U4",
            f"At least 3 keypress events received (got {len(key_lines)})",
            f"Too few keypress events: {len(key_lines)} (need >= 3)",
        )

        # U5: Correct ASCII characters decoded
        decoded_chars = set()
        for line in key_lines:
            # Format: [USB_DEMO] keypress: 'h' sc=0x0b
            if "'" in line:
                parts = line.split("'")
                if len(parts) >= 2 and len(parts[1]) == 1:
                    decoded_chars.add(parts[1])
        expected = {"h", "e", "l"}
        found = expected & decoded_chars
        self.assert_true(
            len(found) >= 2,
            "U5",
            f"Correct ASCII chars decoded: {sorted(found)} (need >= 2 of h,e,l)",
            f"ASCII decode incorrect: got {sorted(decoded_chars)}, "
            f"expected subset of {{h,e,l}}",
        )

        # U6: Demo reports PASS
        self.assert_serial_contains(
            runner,
            "[USB_DEMO] PASS",
            "U6",
            "[USB_DEMO] PASS reported",
        )

        # U7: No kernel panic
        self.assert_true(
            "*** KERNEL PANIC ***" not in log,
            "U7",
            "No kernel panic",
            "Kernel panic detected in serial log",
        )


# ── Custom run() that injects keys via QEMU monitor ──────────────────────────

class UsbTestRunner(UsbTest):
    """Subclass that overrides run() to inject keystrokes mid-boot."""

    def run(self):
        import tempfile
        import shutil

        iso_path = self.iso_path
        if not os.path.exists(iso_path):
            print(f"[SKIP] USB demo ISO not found: {iso_path}")
            print("       Run 'make usb-demo-iso' first.")
            sys.exit(0)

        # Build a temporary disk image
        with tempfile.NamedTemporaryFile(suffix=".img", delete=False) as f:
            disk_path = f.name

        try:
            db = DiskBuilder(disk_path)
            self.setup_disk(db)

            # Monitor socket path
            mon_sock = tempfile.mktemp(suffix=".sock")

            # Serial log file
            serial_log_file = tempfile.mktemp(suffix=".log")

            # QEMU command
            qemu_cmd = [
                "qemu-system-i386",
                "-cdrom",   iso_path,
                "-drive",   f"file={disk_path},format=raw,if=ide,index=1",
                "-boot",    "d",
                "-serial",  f"file:{serial_log_file}",
                "-display", "none",
                "-m",       "64M",
                "-monitor", f"unix:{mon_sock},server,nowait",
                "-no-reboot", "-no-shutdown",
            ] + self.extra_qemu_args()

            # Start QEMU
            proc = subprocess.Popen(
                qemu_cmd,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                stdin=subprocess.DEVNULL,
            )

            # Inject keys in a background thread
            t = threading.Thread(
                target=self._inject_keys_thread,
                args=(mon_sock, INJECT_KEYS, KEY_INJECT_DELAY, KEY_INTER_DELAY),
                daemon=True,
            )
            t.start()

            # Wait for done marker or timeout (30 s)
            deadline = time.time() + 30
            done = False
            while time.time() < deadline:
                time.sleep(0.2)
                if os.path.exists(serial_log_file):
                    with open(serial_log_file, "r", errors="replace") as fh:
                        content = fh.read()
                    for marker in self.done_markers:
                        if marker in content:
                            done = True
                            break
                if done:
                    break

            # Give QEMU a moment to flush serial output
            time.sleep(0.5)
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

            # Read serial log
            serial_lines = []
            if os.path.exists(serial_log_file):
                with open(serial_log_file, "r", errors="replace") as fh:
                    serial_lines = [l.rstrip("\r\n") for l in fh.readlines()]

            # Build a minimal QEMURunner-like object for verify()
            runner = _FakeRunner(serial_lines)

            # Run assertions
            passed = 0
            failed = 0
            results = []

            # Monkey-patch assert methods to collect results
            _orig_assert_true = self.assert_true
            _orig_assert_contains = self.assert_serial_contains

            def _assert_true(cond, code, ok_msg, fail_msg):
                nonlocal passed, failed
                if cond:
                    passed += 1
                    results.append(f"  ✓  [{code}] {ok_msg}")
                else:
                    failed += 1
                    results.append(f"  ✗  [{code}] {fail_msg}")

            def _assert_contains(r, needle, code, ok_msg):
                nonlocal passed, failed
                log_str = "\n".join(r.serial_log)
                if needle in log_str:
                    passed += 1
                    results.append(f"  ✓  [{code}] {ok_msg}")
                else:
                    failed += 1
                    results.append(f"  ✗  [{code}] '{needle}' not found in serial log")

            self.assert_true = _assert_true
            self.assert_serial_contains = _assert_contains

            self.verify(runner, disk_path)

            self.assert_true = _orig_assert_true
            self.assert_serial_contains = _orig_assert_contains

            # Print results
            total = passed + failed
            print(f"\n{'━' * 70}")
            print(f"  {self.scenario_name}")
            print(f"{'━' * 70}")
            for r in results:
                print(r)
            print(f"{'━' * 70}")
            status = "PASS" if failed == 0 else "FAIL"
            print(f"  {status}: {passed}/{total} assertions passed")
            print(f"{'━' * 70}\n")

            sys.exit(0 if failed == 0 else 1)

        finally:
            os.unlink(disk_path)
            for f in [serial_log_file if 'serial_log_file' in dir() else None,
                      mon_sock if 'mon_sock' in dir() else None]:
                if f and os.path.exists(f):
                    try:
                        os.unlink(f)
                    except OSError:
                        pass


class _FakeRunner:
    """Minimal QEMURunner-like object for verify()."""
    def __init__(self, lines):
        self.serial_log = lines

    def contains(self, needle):
        return any(needle in line for line in self.serial_log)


if __name__ == "__main__":
    UsbTestRunner().run()
