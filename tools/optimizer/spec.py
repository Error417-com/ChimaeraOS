#!/usr/bin/env python3
"""
tools/optimizer/spec.py — Parameter Space Specification for ChimaeraOS Optimizer
==================================================================================

This module defines the YAML spec format and parsing logic for tunable parameters.

Each parameter declaration includes:
  - name:         Human-readable identifier (must be unique).
  - type:         One of "int" or "float".
  - bounds:       [lo, hi] inclusive hard box constraints (constitutional limits).
  - default:      Initial value (must be within bounds).
  - build_key:    The kernel build-config key this maps to (e.g., CONFIG_CACHE_TTL).
  - objective:    Either "minimize" or "maximize", specifying the optimization direction.
                  For latency-type metrics, use "minimize"; for throughput, "maximize".
  - metric_name:  The [METRICS] key name emitted by the kernel for this parameter's effect.

The box bounds are constitutional hard limits — they are inputs, not things the
optimizer may alter. This is enforced at parse time.

Example YAML structure:

    parameters:
      - name: cache_ttl_sec
        type: int
        bounds: [1, 3600]
        default: 60
        build_key: CONFIG_CACHE_TTL_SEC
        objective: minimize
        metric_name: cache_miss_rate

      - name: retry_backoff_coeff
        type: float
        bounds: [1.0, 10.0]
        default: 2.0
        build_key: CONFIG_RETRY_BACKOFF_COEFF
        objective: minimize
        metric_name: avg_retry_latency_ms

"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Union

import yaml


@dataclass
class ParameterSpec:
    """A single tunable parameter specification."""

    name: str
    param_type: str  # "int" or "float"
    lo: Union[int, float]
    hi: Union[int, float]
    default: Union[int, float]
    build_key: str
    objective: str  # "minimize" or "maximize"
    metric_name: str

    def __post_init__(self) -> None:
        """Validate the parameter spec after initialization."""
        if self.param_type not in ("int", "float"):
            raise ValueError(
                f"Parameter '{self.name}' has invalid type '{self.param_type}'; "
                f"must be 'int' or 'float'"
            )

        if self.lo > self.hi:
            raise ValueError(
                f"Parameter '{self.name}' has invalid bounds: lo={self.lo} > hi={self.hi}"
            )

        if not (self.lo <= self.default <= self.hi):
            raise ValueError(
                f"Parameter '{self.name}' default={self.default} is outside "
                f"bounds [{self.lo}, {self.hi}]"
            )

        if self.objective not in ("minimize", "maximize"):
            raise ValueError(
                f"Parameter '{self.name}' has invalid objective='{self.objective}'; "
                f"must be 'minimize' or 'maximize'"
            )

    def clamp(self, value: Union[int, float]) -> Union[int, float]:
        """Clamp a value to the parameter's bounds, preserving type."""
        clamped = max(self.lo, min(self.hi, value))
        if self.param_type == "int":
            return int(round(clamped))
        return clamped


@dataclass
class OptimizerSpec:
    """Complete optimizer specification from YAML."""

    parameters: List[ParameterSpec] = field(default_factory=list)
    metadata: Dict[str, Any] = field(default_factory=dict)

    @classmethod
    def from_yaml(cls, yaml_path: Union[str, Path]) -> "OptimizerSpec":
        """Load an optimizer spec from a YAML file."""
        yaml_path = Path(yaml_path).resolve()
        if not yaml_path.exists():
            raise FileNotFoundError(f"YAML spec not found: {yaml_path}")

        with open(yaml_path, "r", encoding="utf-8") as f:
            content = f.read()

        # Check for BOM
        if content.startswith("\ufeff"):
            raise ValueError(f"YAML file contains BOM: {yaml_path}")

        data = yaml.safe_load(content)

        if not isinstance(data, dict):
            raise ValueError("YAML root must be a dictionary")

        spec = cls()
        spec.metadata = data.get("metadata", {})

        params_data = data.get("parameters", [])
        if not isinstance(params_data, list):
            raise ValueError("'parameters' must be a list")

        seen_names = set()
        for i, p in enumerate(params_data):
            if not isinstance(p, dict):
                raise ValueError(f"Parameter {i} must be a dictionary")

            # Validate required fields
            required = ["name", "type", "bounds", "default", "build_key", "objective", "metric_name"]
            missing = [f for f in required if f not in p]
            if missing:
                raise ValueError(f"Parameter {i} missing required fields: {missing}")

            name = p["name"]
            if name in seen_names:
                raise ValueError(f"Duplicate parameter name: '{name}'")
            seen_names.add(name)

            bounds = p["bounds"]
            if not isinstance(bounds, list) or len(bounds) != 2:
                raise ValueError(f"Parameter '{name}' bounds must be [lo, hi]")

            lo, hi = bounds
            param_spec = ParameterSpec(
                name=name,
                param_type=p["type"],
                lo=lo,
                hi=hi,
                default=p["default"],
                build_key=p["build_key"],
                objective=p["objective"],
                metric_name=p["metric_name"],
            )
            spec.parameters.append(param_spec)

        return spec

    def get_param_by_name(self, name: str) -> Optional[ParameterSpec]:
        """Look up a parameter by name."""
        for p in self.parameters:
            if p.name == name:
                return p
        return None

    def get_param_by_build_key(self, build_key: str) -> Optional[ParameterSpec]:
        """Look up a parameter by its build-config key."""
        for p in self.parameters:
            if p.build_key == build_key:
                return p
        return None

    @property
    def param_names(self) -> List[str]:
        """Return list of parameter names."""
        return [p.name for p in self.parameters]

    @property
    def n_dims(self) -> int:
        """Return the dimensionality of the parameter space."""
        return len(self.parameters)
