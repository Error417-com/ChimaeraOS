#!/usr/bin/env python3
"""
acpi_test.py — ACPI shutdown and reboot integration test for ChimeraOS.

Scenario: acpi_test
  Boots the ACPI demo ISO (chimaera_acpi_demo.iso), waits for the serial
  shell to become ready, then:

  Sub-test A (shutdown):
    Sends "shutdown\n" via the QEMU serial socket.
    Verifies ACPI table discovery, PM1_CNT write, and QEMU exit.

  Sub-test B (reboot):
    Sends "reboot\n" via the QEMU serial socket.
    Verifies ACPI table discovery, keyboard-controller reboot path,
    and QEMU exit (with -no-reboot, the reboot causes QEMU to exit).

Assertions
----------
  A1  RSDP found line appears in serial log
  A2  FADT parsed (PM1a_CNT address printed)
  A3  Shell ready marker appears
  A4  "shutdown" command acknowledged
  A5  "[ACPI] Shutdown requested" appears
  A6  PM1_CNT write line appears (confirms ACPI S5 path taken)
  B1  RSDP found in reboot run
  B2  Shell ready in reboot run
  B3  "reboot" command acknowledged
  B4  "[ACPI] Reboot requested" appears
  B5  Reboot method line appears (KBC or reset register)

Exit codes
----------
  0  All assertions passed
  1  One or more assertions failed
  2  Setup error (ISO not found, QEMU not available)
"""

import os
import sys
import time
import socket
import subprocess
import threading

# ── ISO path ──────────────────────────────────────────────────────────────

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.join(_HERE, "..", "..")
ACPI_ISO = os.path.abspath(os.path.join(_REPO, "chimaera_acpi_demo.iso"))

QEMU    = "qemu-system-i386"
TIMEOUT = 30   # seconds

# ── Helpers ───────────────────────────────────────────────────────────────

def ppass(msg): print(f"  [PASS] {msg}")
def pfail(msg): print(f"  [FAIL] {msg}")

# ── Serial reader ─────────────────────────────────────────────────────────

class SerialReader:
    def __init__(self, sock):
        self.sock = sock
        self._log = b""
        self._stop = False
        self._lock = threading.Lock()
        t = threading.Thread(target=self._run, daemon=True)
        t.start()

    def _run(self):
        while not self._stop:
            try:
                data = self.sock.recv(4096)
                if not data:
                    break
                with self._lock:
                    self._log += data
                sys.stdout.write(data.decode("latin-1", errors="replace"))
                sys.stdout.flush()
            except OSError:
                break

    def log(self):
        with self._lock:
            return self._log.decode("latin-1", errors="replace")

    def wait_for(self, text, timeout=TIMEOUT):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if text in self.log():
                return True
            time.sleep(0.05)
        return False

    def stop(self):
        self._stop = True

# ── Boot and run one command ──────────────────────────────────────────────

def run_command(label, command):
    """
    Boot the ACPI demo ISO, wait for shell ready, send `command`,
    wait for QEMU to exit.  Returns the serial log as a string.
    """
    print(f"\n{'─'*70}")
    print(f"  Running: {label} (command={command!r})")
    print(f"{'─'*70}")

    sock_path = f"/tmp/acpi_inttest_{label}.sock"
    if os.path.exists(sock_path):
        os.unlink(sock_path)

    qemu_cmd = [
        QEMU,
        "-m", "128",
        "-cdrom", ACPI_ISO,
        "-boot", "d",
        "-display", "none",
        "-no-reboot",
        "-chardev", f"socket,id=ser,path={sock_path},server=on,wait=off",
        "-serial", "chardev:ser",
    ]

    proc = subprocess.Popen(
        qemu_cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    # Wait for socket
    deadline = time.time() + 10
    while not os.path.exists(sock_path) and time.time() < deadline:
        time.sleep(0.1)

    if not os.path.exists(sock_path):
        proc.kill(); proc.wait()
        return ""

    try:
        ser_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        ser_sock.connect(sock_path)
    except OSError as e:
        print(f"  Socket error: {e}")
        proc.kill(); proc.wait()
        return ""

    reader = SerialReader(ser_sock)

    # Wait for shell
    if not reader.wait_for("[ACPI_DEMO] shell ready", timeout=TIMEOUT):
        print("  TIMEOUT waiting for shell ready")
        proc.kill(); proc.wait()
        reader.stop(); ser_sock.close()
        return reader.log()

    time.sleep(0.3)
    try:
        ser_sock.sendall((command + "\n").encode())
    except OSError:
        pass

    # Wait for QEMU to exit (shutdown/reboot both terminate with -no-reboot)
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        print("  TIMEOUT: QEMU did not exit")
        proc.kill(); proc.wait()

    time.sleep(0.3)
    log = reader.log()
    reader.stop()
    ser_sock.close()
    if os.path.exists(sock_path):
        os.unlink(sock_path)
    return log

# ── Main ──────────────────────────────────────────────────────────────────

def main():
    print("=" * 70)
    print("Scenario: acpi_test")
    print("  ACPI shutdown and reboot via ACPI S5 / keyboard controller")
    print("=" * 70)

    # Pre-flight
    if not os.path.exists(ACPI_ISO):
        print(f"ERROR: ACPI demo ISO not found: {ACPI_ISO}")
        print("Run: cd chimera_os && make acpi-demo-iso")
        sys.exit(2)

    if subprocess.call(["which", QEMU],
                       stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL) != 0:
        print(f"ERROR: {QEMU} not found")
        sys.exit(2)

    total_pass = 0
    total_fail = 0

    def check(cid, desc, needle, log):
        nonlocal total_pass, total_fail
        if needle in log:
            ppass(f"{cid}: {desc}")
            total_pass += 1
        else:
            pfail(f"{cid}: {desc}  (expected: {needle!r})")
            total_fail += 1

    # ── Sub-test A: shutdown ───────────────────────────────────────────────
    log_a = run_command("shutdown", "shutdown")

    print("\n  Shutdown assertions:")
    check("A1", "RSDP found",        "[ACPI] RSDP found at",        log_a)
    check("A2", "FADT parsed",       "[ACPI] FADT at",              log_a)
    check("A3", "Shell ready",       "[ACPI_DEMO] shell ready",     log_a)
    check("A4", "Shutdown cmd ack",  "[ACPI_DEMO] cmd: shutdown",   log_a)
    check("A5", "Shutdown requested","[ACPI] Shutdown requested",   log_a)
    check("A6", "PM1_CNT write",     "[ACPI] Writing",              log_a)

    # ── Sub-test B: reboot ────────────────────────────────────────────────
    log_b = run_command("reboot", "reboot")

    print("\n  Reboot assertions:")
    check("B1", "RSDP found",        "[ACPI] RSDP found at",        log_b)
    check("B2", "Shell ready",       "[ACPI_DEMO] shell ready",     log_b)
    check("B3", "Reboot cmd ack",    "[ACPI_DEMO] cmd: reboot",     log_b)
    check("B4", "Reboot requested",  "[ACPI] Reboot requested",     log_b)
    check("B5", "Reboot method",     "Reboot via",                  log_b)

    # ── Summary ───────────────────────────────────────────────────────────
    total = total_pass + total_fail
    print(f"\n{'━'*70}")
    print(f"  PASS: {total_pass}/{total} assertions passed")
    print(f"{'━'*70}")

    if total_fail == 0:
        print("  ✓ PASS  acpi_test")
        sys.exit(0)
    else:
        print("  ✗ FAIL  acpi_test")
        sys.exit(1)

if __name__ == "__main__":
    main()
