#!/usr/bin/env python3
"""
tests/integration/metrics_test.py
==================================
Integration test: Kernel Metrics Subsystem

Boots the integration kernel with scenario 'M', verifies that all six
[METRICS] key=value lines are present and contain plausible values, and
checks that [METRICS] DONE is emitted.

Assertions
----------
  MT1  [METRICS] READY line present
  MT2  boot_ms is present and > 0
  MT3  mem_after_boot_bytes is present and > 0
  MT4  ttfb_ms is present and >= 0
  MT5  tokens_per_sec is present and > 0
  MT6  mem_after_infer_bytes >= mem_after_boot_bytes
  MT7  [METRICS] DONE line present
"""

import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "lib"))

from qemu_runner import IntegrationTest, DiskBuilder, QEMURunner


class MetricsTest(IntegrationTest):
    scenario_id   = "metrics_test"
    scenario_name = "Kernel metrics subsystem — boot_ms, mem, ttfb, tokens/sec"
    qemu_extra    = []
    done_markers  = ["[INTTEST] DONE"]

    def setup_disk(self, db: DiskBuilder) -> None:
        db.write_file("SCENARIO", b"M")

    def verify(self, runner: QEMURunner, disk_path: str) -> None:
        # MT1: READY line
        self.assert_serial_contains(
            runner,
            "[METRICS] READY",
            "MT1",
            "[METRICS] READY line present",
        )

        # Parse all [METRICS] key=value lines from the serial log
        metrics = {}
        for line in runner.serial_log:
            line = line.strip()
            if line.startswith("[METRICS] ") and "=" in line:
                kv = line[len("[METRICS] "):]
                k, _, v = kv.partition("=")
                try:
                    metrics[k.strip()] = int(v.strip())
                except ValueError:
                    pass

        # MT2: boot_ms
        boot_ms = metrics.get("boot_ms", -1)
        self.assert_true(
            boot_ms > 0,
            "MT2",
            f"boot_ms={boot_ms} > 0",
            f"boot_ms={boot_ms} is not > 0 (missing or zero)",
        )

        # MT3: mem_after_boot_bytes
        mem_boot = metrics.get("mem_after_boot_bytes", -1)
        self.assert_true(
            mem_boot > 0,
            "MT3",
            f"mem_after_boot_bytes={mem_boot} > 0",
            f"mem_after_boot_bytes={mem_boot} is not > 0",
        )

        # MT4: ttfb_ms (>= 0; simulation may produce 0 on very fast hosts)
        ttfb = metrics.get("ttfb_ms", -1)
        self.assert_true(
            ttfb >= 0,
            "MT4",
            f"ttfb_ms={ttfb} >= 0",
            f"ttfb_ms={ttfb} is missing or negative",
        )

        # MT5: tokens_per_sec
        tps = metrics.get("tokens_per_sec", -1)
        self.assert_true(
            tps > 0,
            "MT5",
            f"tokens_per_sec={tps} > 0",
            f"tokens_per_sec={tps} is not > 0",
        )

        # MT6: mem_after_infer >= mem_after_boot
        mem_infer = metrics.get("mem_after_infer_bytes", -1)
        self.assert_true(
            mem_infer >= mem_boot,
            "MT6",
            f"mem_after_infer_bytes={mem_infer} >= mem_after_boot_bytes={mem_boot}",
            f"mem_after_infer_bytes={mem_infer} < mem_after_boot_bytes={mem_boot}",
        )

        # MT7: DONE line
        self.assert_serial_contains(
            runner,
            "[METRICS] DONE",
            "MT7",
            "[METRICS] DONE line present",
        )

        # Print the metrics block for human inspection
        print("\n  --- Metrics output (for human inspection) ---")
        for ln in runner.serial_log:
            if "[METRICS]" in ln or "[SYMTAB]" in ln:
                print(f"    {ln}")
        print("  --- End of metrics output ---")
        print(f"\n  Parsed metrics: {metrics}")


if __name__ == "__main__":
    sys.exit(MetricsTest().run())
