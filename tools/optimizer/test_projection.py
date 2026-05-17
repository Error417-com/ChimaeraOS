#!/usr/bin/env python3
"""
tools/optimizer/test_projection.py — Unit Tests for Projection Function
========================================================================

Property tests for the projection function:
  1. Feasibility: For any random point in ℝⁿ, the projected point is provably
     inside the box [lo, hi].
  2. Nearest-point: The projected point is the nearest point in the box to the
     original point (under Euclidean distance).
  3. Idempotence: If a point is already in the box, projection returns it unchanged
     with distance 0.

These tests verify the core safety property: projection ⊋ filtering.
"""

from __future__ import annotations

import math
import random
import sys
from pathlib import Path
from typing import List

# Add repo root to path
REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from tools.optimizer.projection import project_to_box


def test_feasibility_random_points(num_trials: int = 1000) -> None:
    """
    Property test: For any random point in ℝⁿ, the projected point is inside the box.

    We generate random boxes and random points, then verify that every coordinate
    of the projected point satisfies lo[i] ≤ projected[i] ≤ hi[i].
    """
    print("Running feasibility test...")

    for trial in range(num_trials):
        # Random dimensionality (1 to 10)
        n = random.randint(1, 10)

        # Random bounds: lo in [-100, 0], hi in [0, 100]
        lo = [random.uniform(-100, 0) for _ in range(n)]
        hi = [lo[i] + random.uniform(0.1, 100) for i in range(n)]

        # Random point in ℝⁿ: uniform in [-200, 200]
        point = [random.uniform(-200, 200) for _ in range(n)]

        # Project
        projected, distance = project_to_box(point, lo, hi)

        # Verify feasibility
        for i in range(n):
            assert lo[i] <= projected[i] <= hi[i], (
                f"Feasibility violation at trial {trial}, dim {i}: "
                f"lo={lo[i]}, hi={hi[i]}, projected={projected[i]}"
            )

    print(f"  ✓ Feasibility test passed ({num_trials} trials)")


def test_nearest_point_property(num_trials: int = 100) -> None:
    """
    Property test: The projected point is the nearest point in the box.

    For box constraints, the nearest point is found by independent clamping.
    We verify this by checking that the distance returned matches the manually
    computed Euclidean distance, and that no other point in the box is closer.
    """
    print("Running nearest-point test...")

    for trial in range(num_trials):
        n = random.randint(1, 5)

        # Random bounds
        lo = [random.uniform(-50, 0) for _ in range(n)]
        hi = [lo[i] + random.uniform(1, 50) for i in range(n)]

        # Random point outside the box (to make the test meaningful)
        point = []
        for i in range(n):
            if random.random() < 0.5:
                # Below the box
                point.append(lo[i] - random.uniform(1, 50))
            else:
                # Above the box
                point.append(hi[i] + random.uniform(1, 50))

        # Project
        projected, distance = project_to_box(point, lo, hi)

        # Manually compute expected distance
        expected_sq_dist = sum((point[i] - projected[i]) ** 2 for i in range(n))
        expected_distance = math.sqrt(expected_sq_dist)

        # Verify distance matches
        assert abs(distance - expected_distance) < 1e-9, (
            f"Distance mismatch at trial {trial}: "
            f"got {distance}, expected {expected_distance}"
        )

        # Verify no point in the box is closer (by sampling random points in box)
        for _ in range(10):
            random_in_box = [random.uniform(lo[i], hi[i]) for i in range(n)]
            dist_to_random = math.sqrt(sum((point[i] - random_in_box[i]) ** 2 for i in range(n)))
            assert dist_to_random >= distance - 1e-9, (
                f"Found closer point at trial {trial}: "
                f"projected distance={distance}, random point distance={dist_to_random}"
            )

    print(f"  ✓ Nearest-point test passed ({num_trials} trials)")


def test_idempotence(num_trials: int = 100) -> None:
    """
    Property test: If a point is already in the box, projection is idempotent.

    When point ∈ [lo, hi], we expect projected == point and distance == 0.
    """
    print("Running idempotence test...")

    for trial in range(num_trials):
        n = random.randint(1, 10)

        # Random bounds
        lo = [random.uniform(-100, 0) for _ in range(n)]
        hi = [lo[i] + random.uniform(1, 100) for i in range(n)]

        # Point inside the box
        point = [random.uniform(lo[i], hi[i]) for i in range(n)]

        # Project
        projected, distance = project_to_box(point, lo, hi)

        # Verify identity
        for i in range(n):
            assert projected[i] == point[i], (
                f"Idempotence violation at trial {trial}, dim {i}: "
                f"original={point[i]}, projected={projected[i]}"
            )

        # Verify zero distance
        assert distance == 0.0, (
            f"Non-zero distance for in-box point at trial {trial}: {distance}"
        )

    print(f"  ✓ Idempotence test passed ({num_trials} trials)")


def test_boundary_cases() -> None:
    """
    Test edge cases: points exactly on the boundary.
    """
    print("Running boundary cases test...")

    # 1D case: point exactly at lower bound
    projected, distance = project_to_box([5.0], [5.0], [10.0])
    assert projected == [5.0]
    assert distance == 0.0

    # 1D case: point exactly at upper bound
    projected, distance = project_to_box([10.0], [5.0], [10.0])
    assert projected == [10.0]
    assert distance == 0.0

    # 1D case: point just outside lower bound
    projected, distance = project_to_box([4.999], [5.0], [10.0])
    assert projected == [5.0]
    assert abs(distance - 0.001) < 1e-9  # Use tolerance for floating-point

    # 1D case: point just outside upper bound
    projected, distance = project_to_box([10.001], [5.0], [10.0])
    assert projected == [10.0]
    assert abs(distance - 0.001) < 1e-9  # Use tolerance for floating-point

    # Multi-dimensional: mixed inside/outside
    projected, distance = project_to_box([3.0, 7.0, 15.0], [5.0, 0.0, 10.0], [10.0, 10.0, 20.0])
    assert projected == [5.0, 7.0, 15.0]  # First clamped up, second unchanged, third unchanged
    expected_dist = math.sqrt((3.0 - 5.0) ** 2)  # Only first dimension contributes
    assert abs(distance - expected_dist) < 1e-9

    print("  ✓ Boundary cases test passed")


def test_error_handling() -> None:
    """
    Test that invalid inputs raise appropriate errors.
    """
    print("Running error handling test...")

    # Mismatched lengths
    try:
        project_to_box([1.0, 2.0], [1.0], [3.0])
        assert False, "Should have raised ValueError for mismatched lengths"
    except ValueError as e:
        assert "same length" in str(e)

    # Invalid bounds (lo > hi)
    try:
        project_to_box([5.0], [10.0], [1.0])
        assert False, "Should have raised ValueError for lo > hi"
    except ValueError as e:
        assert "Invalid bounds" in str(e)

    print("  ✓ Error handling test passed")


def run_all_tests() -> int:
    """Run all tests and return exit code."""
    print("=" * 70)
    print("tools/optimizer/test_projection.py — Projection Function Tests")
    print("=" * 70)
    print()

    try:
        test_feasibility_random_points()
        test_nearest_point_property()
        test_idempotence()
        test_boundary_cases()
        test_error_handling()

        print()
        print("=" * 70)
        print("All tests PASSED")
        print("=" * 70)
        return 0

    except AssertionError as e:
        print()
        print("=" * 70)
        print(f"Test FAILED: {e}")
        print("=" * 70)
        return 1


if __name__ == "__main__":
    sys.exit(run_all_tests())
