# ChimaeraOS Parameter Optimizer

## Overview

This package implements a **host-side projected gradient-descent optimizer** for ChimaeraOS bounded auto-promote parameters. It is a "safe by construction" mechanism for the subset of Article III parameters whose constraints are a-priori bounded (provably evaluable without running the kernel on real traffic).

## Safety Claim

> **Every candidate point is projected into the constitutional box constraints BEFORE it can be built or evaluated.** An out-of-box point is unrepresentable as a built candidate, not rejected after building.

### Why Projection ⊋ Filtering

A filter operates post-hoc: it enumerates candidates and discards those that violate constraints. This admits enumeration gaps — there may exist valid points that are never generated due to discretization or sampling bias, and invalid points that slip through if the filter logic is incomplete.

By contrast, a **projection** is a mathematical mapping P: ℝⁿ → C (where C is the feasible box) that guarantees:
1. **Feasibility**: For any x ∈ ℝⁿ, P(x) ∈ C.
2. **Nearest-point**: P(x) = argmin_{y∈C} ||x - y||₂.

The projection cannot leave the feasible set; it is a closure operation. This is the core "safe by construction" mechanism.

## Residual Trust Anchor

The safety guarantee assumes the **objective function is correctly specified per parameter** in the YAML spec. If the objective (e.g., "latency / miss-rate") does not match the intended optimization goal, the optimizer will converge to a technically correct but semantically wrong solution. This mirrors the eval-coverage problem: we can prove the mechanism is sound, but not that the specification is complete.

## Deterministic Synthetic Workload

All evaluations use a deterministic synthetic workload (no real user traffic). This is why cache-TTL and retry-backoff parameters qualify for auto-promotion: their effects are provably evaluable without running on real traffic.

**Planner heuristics**, by contrast, are irreducibly empirical and explicitly excluded from this optimizer (gated/human path only).

## Included Parameters

| Parameter | Type | Bounds | Build Key | Objective | Metric |
|-----------|------|--------|-----------|-----------|--------|
| `cache_ttl_sec` | int | [1, 3600] | CONFIG_CACHE_TTL_SEC | minimize | cache_miss_rate |
| `retry_backoff_coeff` | float | [1.0, 10.0] | CONFIG_RETRY_BACKOFF_COEFF | minimize | avg_retry_latency_ms |

## Explicitly Excluded (Per Constitution)

- Internal planner heuristics (irreducibly empirical, gated/human path only)
- Logging verbosity (immutable exclusion)

## Usage

### Run the Optimizer (A2 Recommend Mode)

```bash
cd /workspace
python3 tools/optimizer/recommend.py \
    --spec tools/optimizer/params.example.yaml \
    --max-iterations 10
```

This will:
1. Load the parameter spec from YAML.
2. Run projected gradient descent.
3. Output the best in-box point found.
4. Generate a unified diff of the recommended build-config change.
5. Save an append-only JSONL audit log for rollback verification.

**Note**: This is A2 recommend mode — it does NOT promote changes automatically. The output is for the gated/human path.

### Run Unit Tests

```bash
python3 tools/optimizer/test_projection.py
```

This runs property tests verifying:
- Feasibility: projected points are always inside the box.
- Nearest-point: projection returns the closest point in the box.
- Idempotence: in-box points are unchanged by projection.

## Package Structure

```
tools/optimizer/
├── __init__.py           # Package marker
├── projection.py         # Core projection function (P: ℝⁿ → C)
├── spec.py               # YAML spec parsing and validation
├── core.py               # Optimization loop implementation
├── recommend.py          # Main entry point (A2 recommend mode)
├── test_projection.py    # Unit tests for projection
├── params.example.yaml   # Example parameter specification
└── README.md             # This file
```

## Audit Trail

Every evaluation is logged to an append-only JSONL file with:
- Parameter values
- Objective value
- Raw metrics from [METRICS] output
- SHA256 hash of the built kernel image
- ISO-8601 UTC timestamp
- Iteration number

This provides the complete trace required for constitution-mandated rollback.

## Out of Scope

The following are explicitly out of scope for this task:
- Kernel modification
- Paging or ring 0→3 transitions
- Planner-heuristic tuning
- Auto-promotion wiring
- Any LLM/ML components

If any of these seem required, the boundary has been crossed.
