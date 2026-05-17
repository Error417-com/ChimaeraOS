#!/usr/bin/env python3
"""
tools/optimizer/recommend.py — A2 Recommend Mode for ChimaeraOS Optimizer
==========================================================================

This is the main entry point for running the optimizer in "A2 recommend mode":
it does NOT promote changes automatically. Instead, it emits:
  1. The best in-box point found during optimization.
  2. A unified diff of the build-config change.
  3. The full audit trail (append-only JSONL) for rollback verification.

This implements the gated/human path required by the constitution.
Auto-promotion wiring is explicitly out of scope for this task.

Usage
-----
    python3 tools/optimizer/recommend.py --spec PATH/to/spec.yaml \
        [--output-dir DIR] [--max-iterations N] [--learning-rate LR]

Example
-------
    python3 tools/optimizer/recommend.py \
        --spec tools/optimizer/params.example.yaml \
        --output-dir /tmp/opt_run_001
"""

from __future__ import annotations

import argparse
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

# Add repo root to path
REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from tools.optimizer.core import Optimizer, OptimizationResult
from tools.optimizer.spec import OptimizerSpec


def parse_args() -> argparse.Namespace:
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(
        description="ChimaeraOS Parameter Optimizer (A2 Recommend Mode)"
    )
    parser.add_argument(
        "--spec",
        type=Path,
        required=True,
        help="Path to YAML parameter specification file",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="Output directory for results (default: timestamped dir under optimizer/)",
    )
    parser.add_argument(
        "--max-iterations",
        type=int,
        default=20,
        help="Maximum number of optimization iterations",
    )
    parser.add_argument(
        "--learning-rate",
        type=float,
        default=0.1,
        help="Gradient descent learning rate",
    )
    parser.add_argument(
        "--initial-params",
        type=str,
        default=None,
        help="JSON dict of initial parameter values (overrides defaults)",
    )
    return parser.parse_args()


def main() -> int:
    """Main entry point."""
    args = parse_args()

    # Validate spec file exists
    if not args.spec.exists():
        print(f"ERROR: Spec file not found: {args.spec}", file=sys.stderr)
        return 1

    # Load spec
    print(f"Loading parameter spec from {args.spec}...")
    try:
        spec = OptimizerSpec.from_yaml(args.spec)
    except Exception as e:
        print(f"ERROR: Failed to load spec: {e}", file=sys.stderr)
        return 1

    print(f"  Loaded {len(spec.parameters)} parameters:")
    for p in spec.parameters:
        print(f"    - {p.name}: {p.param_type} in [{p.lo}, {p.hi}], default={p.default}")

    # Prepare output directory
    if args.output_dir is None:
        timestamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
        output_dir = REPO_ROOT / "tools" / "optimizer" / "runs" / f"run_{timestamp}"
    else:
        output_dir = args.output_dir.resolve()

    output_dir.mkdir(parents=True, exist_ok=True)
    audit_log_path = output_dir / "audit.jsonl"
    result_path = output_dir / "result.txt"
    diff_path = output_dir / "recommendation.diff"

    print(f"\nOutput directory: {output_dir}")

    # Parse initial params if provided
    initial_params = None
    if args.initial_params:
        import json
        try:
            initial_params = json.loads(args.initial_params)
            print(f"Using initial params: {initial_params}")
        except json.JSONDecodeError as e:
            print(f"ERROR: Invalid JSON for --initial-params: {e}", file=sys.stderr)
            return 1

    # Create and run optimizer
    print(f"\nStarting optimization (max_iterations={args.max_iterations})...")
    optimizer = Optimizer(
        spec=spec,
        audit_log_path=audit_log_path,
        learning_rate=args.learning_rate,
        max_iterations=args.max_iterations,
    )

    try:
        result = optimizer.run(initial_params=initial_params)
    except KeyboardInterrupt:
        print("\nOptimization interrupted by user")
        # Still save partial results
        if optimizer.evaluations:
            print(f"Saving partial results ({len(optimizer.evaluations)} evaluations)...")
            result = OptimizationResult(
                best_params=optimizer.evaluations[0].params,
                best_objective=optimizer.evaluations[0].objective,
                total_iterations=len(optimizer.evaluations),
                evaluations=optimizer.evaluations,
                recommendation_diff="",
            )
        else:
            print("No evaluations completed; exiting")
            return 1
    except Exception as e:
        print(f"ERROR: Optimization failed: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        return 1

    # Save results
    print(f"\n{'='*70}")
    print("OPTIMIZATION COMPLETE")
    print(f"{'='*70}")

    print(f"\nBest objective: {result.best_objective:.4f}")
    print(f"Best parameters:")
    for name, value in result.best_params.items():
        p = spec.get_param_by_name(name)
        if p and p.param_type == "int":
            print(f"  {name} = {int(round(value))}")
        else:
            print(f"  {name} = {value:.6f}")

    print(f"\nTotal iterations: {result.total_iterations}")
    print(f"Total evaluations: {len(result.evaluations)}")

    # Write recommendation diff
    with open(diff_path, "w", encoding="utf-8") as f:
        f.write(result.recommendation_diff)
    print(f"\nRecommendation diff saved to: {diff_path}")
    print("\n--- BEGIN DIFF ---")
    print(result.recommendation_diff)
    print("--- END DIFF ---")

    # Write summary result
    with open(result_path, "w", encoding="utf-8") as f:
        f.write(f"ChimaeraOS Optimizer Result\n")
        f.write(f"===========================\n\n")
        f.write(f"Timestamp: {datetime.now(timezone.utc).isoformat()}\n")
        f.write(f"Spec: {args.spec}\n\n")
        f.write(f"Best Objective: {result.best_objective:.6f}\n\n")
        f.write(f"Best Parameters:\n")
        for name, value in result.best_params.items():
            f.write(f"  {name} = {value}\n")
        f.write(f"\nTotal Iterations: {result.total_iterations}\n")
        f.write(f"Total Evaluations: {len(result.evaluations)}\n")
        f.write(f"\nAudit Log: {audit_log_path}\n")
        f.write(f"Diff: {diff_path}\n")

    print(f"\nFull results saved to: {result_path}")
    print(f"Audit log (append-only JSONL): {audit_log_path}")

    # Safety reminder
    print(f"\n{'='*70}")
    print("A2 RECOMMEND MODE — NO AUTO-PROMOTION")
    print(f"{'='*70}")
    print("""
The optimizer has identified the above parameter configuration as optimal.
To apply these changes:

  1. Review the recommendation diff at: {diff_path}
  2. Manually update the kernel build config (src/kernel/config.h)
  3. Rebuild the kernel: make integration-test-iso
  4. Verify via integration tests: python3 tests/integration/metrics_test.py

The audit log provides a complete trace for rollback if needed.
""".format(diff_path=diff_path))

    return 0


if __name__ == "__main__":
    sys.exit(main())
