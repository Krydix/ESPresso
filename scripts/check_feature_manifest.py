#!/usr/bin/env python3
"""Validate and summarize ESPresso's executable compatibility contract."""

from __future__ import annotations

import json
import re
import sys
from collections import Counter
from pathlib import Path


ID_PATTERN = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")


def fail(message: str) -> None:
    raise SystemExit(f"feature matrix: {message}")


def main() -> None:
    if len(sys.argv) != 2:
        fail("usage: check_feature_manifest.py tests/feature-matrix.json")
    path = Path(sys.argv[1]).resolve()
    root = path.parent.parent
    data = json.loads(path.read_text())
    if data.get("schema") != 1:
        fail("unsupported or missing schema")
    statuses = set(data.get("statuses", []))
    required_statuses = {"supported", "expected-fail", "hardware-required",
                         "out-of-scope"}
    if statuses != required_statuses:
        fail(f"statuses must be exactly {sorted(required_statuses)}")

    seen = set()
    features = data.get("features")
    if not isinstance(features, list) or not features:
        fail("features must be a non-empty list")
    for index, feature in enumerate(features):
        prefix = f"feature {index + 1}"
        feature_id = feature.get("id", "")
        if not ID_PATTERN.fullmatch(feature_id):
            fail(f"{prefix} has invalid id {feature_id!r}")
        if feature_id in seen:
            fail(f"duplicate id {feature_id}")
        seen.add(feature_id)
        if not feature.get("title") or not isinstance(feature.get("phase"), int):
            fail(f"{feature_id} needs a title and integer phase")
        status = feature.get("status")
        if status not in statuses:
            fail(f"{feature_id} has invalid status {status!r}")
        if status == "supported":
            if not feature.get("target") or not feature.get("evidence"):
                fail(f"{feature_id} needs a target and evidence")
            for relative in feature["evidence"]:
                if not (root / relative).exists():
                    fail(f"{feature_id} evidence does not exist: {relative}")
        elif status == "expected-fail":
            if not feature.get("plannedTarget") or not feature.get("acceptance"):
                fail(f"{feature_id} needs a plannedTarget and acceptance criteria")
        elif not feature.get("reason"):
            fail(f"{feature_id} needs an explicit reason")

    counts = Counter(feature["status"] for feature in features)
    print(f"Feature matrix: {len(features)} declared capabilities [PASS]")
    for status in ("supported", "expected-fail", "hardware-required", "out-of-scope"):
        print(f"  {status}: {counts[status]}")


if __name__ == "__main__":
    main()
