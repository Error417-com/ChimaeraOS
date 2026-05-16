"""
ChimaeraOS — Scheduler Demo Integration Test
tests/integration/sched_test.py

Boots the sched-demo ISO and verifies that two tasks print alternately
without either one calling yield() explicitly.
"""

import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "lib"))

from qemu_runner import IntegrationTest, QEMURunner, DiskBuilder


SCHED_ISO = os.path.join(
    os.path.dirname(__file__), "..", "..", "chimaera_sched_demo.iso"
)


class SchedTest(IntegrationTest):
    scenario_id   = "sched_test"
    scenario_name = "Preemptive scheduler — two tasks print alternately without yield()"
    done_markers  = ["[SCHED_DEMO] PASS", "[SCHED_DEMO] FAIL", "System halted."]

    def __init__(self):
        super().__init__(iso_path=os.path.abspath(SCHED_ISO))

    def setup_disk(self, db: DiskBuilder) -> None:
        # The sched demo kernel does not use the disk at all.
        # We still need a valid FAT32 disk for the ATA driver to find.
        db.write_file("SCENARIO", b"S")

    def verify(self, runner: QEMURunner, disk_path: str) -> None:
        # serial_log is a List[str]; join for substring searches and splitlines
        log = "\n".join(runner.serial_log)

        # S1: Task A printed at least 5 times
        task_a_lines = [l for l in log.splitlines() if "[TASK A]" in l]
        self.assert_true(
            len(task_a_lines) >= 5,
            "S1",
            f"[TASK A] appeared {len(task_a_lines)} times (need >= 5)",
            f"[TASK A] only appeared {len(task_a_lines)} times",
        )

        # S2: Task B printed at least 5 times
        task_b_lines = [l for l in log.splitlines() if "[TASK B]" in l]
        self.assert_true(
            len(task_b_lines) >= 5,
            "S2",
            f"[TASK B] appeared {len(task_b_lines)} times (need >= 5)",
            f"[TASK B] only appeared {len(task_b_lines)} times",
        )

        # S3: Tasks alternate — no run of more than 3 consecutive lines from
        #     the same task (quantum = 50 ms, busy-wait = 200 ms → ~4 ticks
        #     per iteration, so at most 1 tick before preemption).
        #     We allow a run of up to 3 to tolerate startup jitter.
        task_lines = [l for l in log.splitlines()
                      if "[TASK A]" in l or "[TASK B]" in l]
        max_run = 1
        cur_run = 1
        for i in range(1, len(task_lines)):
            prev_a = "[TASK A]" in task_lines[i - 1]
            curr_a = "[TASK A]" in task_lines[i]
            if prev_a == curr_a:
                cur_run += 1
                max_run = max(max_run, cur_run)
            else:
                cur_run = 1
        self.assert_true(
            max_run <= 3,
            "S3",
            f"Tasks alternate (max consecutive run = {max_run})",
            f"Tasks did NOT alternate — max consecutive run = {max_run} (expected <= 3)",
        )

        # S4: Scheduler reported all tasks complete
        self.assert_serial_contains(
            runner,
            "[SCHED] All tasks complete",
            "S4",
            "[SCHED] All tasks complete line present",
        )

        # S5: Demo kernel reported PASS
        self.assert_serial_contains(
            runner,
            "[SCHED_DEMO] PASS",
            "S5",
            "[SCHED_DEMO] PASS line present",
        )

        # S6: No FAIL lines
        self.assert_true(
            "[SCHED_DEMO] FAIL" not in log,
            "S6",
            "No [SCHED_DEMO] FAIL lines",
            "[SCHED_DEMO] FAIL line found in serial log",
        )

        # S7: No kernel panic
        self.assert_true(
            "PANIC" not in log,
            "S7",
            "No kernel panic",
            "Kernel panic detected in serial log",
        )


if __name__ == "__main__":
    SchedTest().run()
