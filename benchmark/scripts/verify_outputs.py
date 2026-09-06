"""Verify native correctness results and measured-run completeness."""

from __future__ import annotations

import argparse
import json
from collections import defaultdict
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("result_dir", type=Path)
    parser.add_argument("--required-runs", type=int, default=5)
    parser.add_argument("--require-artifacts", action="store_true")
    args = parser.parse_args()
    failures: list[str] = []
    measured: dict[tuple[str, str], int] = defaultdict(int)
    checked = 0

    for path in sorted(args.result_dir.rglob("*.json")):
        try:
            payload = json.loads(path.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError) as error:
            failures.append(f"{path}: unreadable JSON ({error})")
            continue
        if "experiment" not in payload and "timing" not in payload:
            continue
        checked += 1
        validation = payload.get("validation", {})
        if payload.get("status") != "success":
            failures.append(f"{path}: status is {payload.get('status')!r}")
        if validation.get("passed") is not True or validation.get("pixel_match") is not True:
            failures.append(f"{path}: complete decoded pixel buffer did not match")
        input_data = payload.get("input", {})
        output_data = payload.get("output", {})
        try:
            expected_bmp_bytes = 54 + int(input_data.get("width", 0)) * int(input_data.get("height", 0)) * 4
            if int(output_data.get("bmp_bytes", -1)) != expected_bmp_bytes:
                failures.append(f"{path}: equivalent BMP size is inconsistent with input dimensions")
            qoi_bytes = int(output_data.get("bytes", 0))
            expected_ratio = expected_bmp_bytes / qoi_bytes if qoi_bytes > 0 else 0.0
            actual_ratio = float(output_data.get("compression_ratio", 0.0))
            if abs(actual_ratio - expected_ratio) > 0.01:
                failures.append(f"{path}: BMP/QOI compression ratio is inconsistent")
        except (TypeError, ValueError, OverflowError):
            failures.append(f"{path}: invalid BMP/QOI size metrics")
        output_path = Path(payload.get("output", {}).get("path", ""))
        if args.require_artifacts and not output_path.is_file():
            failures.append(f"{path}: QOI output is missing ({output_path})")
        experiment = payload.get("experiment", {})
        if not experiment.get("is_warmup", False):
            key = (str(experiment.get("image_id", path.parent.parent.name)),
                   str(experiment.get("configuration_id", path.parent.name)))
            measured[key] += 1

    for key, count in measured.items():
        if count != args.required_runs:
            failures.append(f"{key[0]}/{key[1]}: expected {args.required_runs} measured runs, found {count}")
    if checked == 0:
        failures.append("no native result artifacts found")
    if failures:
        for failure in failures:
            print(f"invalid: {failure}")
        return 1
    print(f"verified {checked} result files across {len(measured)} measured configurations")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
