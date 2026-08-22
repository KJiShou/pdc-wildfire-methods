"""Aggregate measured QOI runs into reproducible per-image and suite statistics."""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable


TIMING_FIELDS = (
    "load_ms", "cuda_init_ms", "allocation_ms", "summary_ms", "propagation_ms", "transfer_in_ms", "encode_ms",
    "transfer_out_ms", "merge_ms", "prefix_scan_ms", "compaction_ms", "core_pipeline_ms", "write_ms", "metrics_analysis_ms", "validation_ms", "total_ms",
)
DERIVED_PHASE_FIELDS = ("pass1_ms", "pass2_ms", "communication_ms")
CHUNK_FIELDS = ("run", "index", "diff", "luma", "rgb", "rgba")
BOOLEAN_FIELDS = ("is_warmup", "validation_passed", "pixel_match", "sha256_match")


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames: list[str] = []
    for row in rows:
        for key in row:
            if key not in fieldnames:
                fieldnames.append(key)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames or ["status"])
        writer.writeheader()
        writer.writerows(rows)


def read_per_run_csv(path: Path) -> list[dict[str, Any]]:
    """Read an existing flattened run table without losing boolean semantics."""
    with path.open("r", newline="", encoding="utf-8-sig") as handle:
        rows: list[dict[str, Any]] = list(csv.DictReader(handle))
    for row in rows:
        for field in BOOLEAN_FIELDS:
            value = str(row.get(field, "")).strip().lower()
            row[field] = True if value == "true" else False if value == "false" else None
    return rows


def finite(values: Iterable[Any]) -> list[float]:
    result = []
    for value in values:
        try:
            number = float(value)
        except (TypeError, ValueError):
            continue
        if math.isfinite(number):
            result.append(number)
    return result


def describe(values: Iterable[Any], prefix: str) -> dict[str, float | None]:
    numbers = finite(values)
    if not numbers:
        return {f"{prefix}_median": None, f"{prefix}_mean": None, f"{prefix}_stdev": None}
    return {
        f"{prefix}_median": statistics.median(numbers),
        f"{prefix}_mean": statistics.fmean(numbers),
        f"{prefix}_stdev": statistics.stdev(numbers) if len(numbers) > 1 else 0.0,
    }


def flatten(path: Path, payload: dict[str, Any]) -> dict[str, Any]:
    experiment = payload.get("experiment", {})
    configuration = {**experiment.get("configuration", {}), **payload.get("configuration", {})}
    timing = payload.get("timing", {})
    output = payload.get("output", {})
    validation = payload.get("validation", {})
    chunks = payload.get("chunks", {})
    cross_block = payload.get("cross_block", {})
    width = payload.get("input", {}).get("width", 0)
    height = payload.get("input", {}).get("height", 0)
    row: dict[str, Any] = {
        "file": str(path),
        "status": payload.get("status"),
        "backend": payload.get("backend"),
        "stage": experiment.get("stage"),
        "image_id": experiment.get("image_id", path.parent.parent.name),
        "category": experiment.get("category", "unclassified"),
        "channel_mode": experiment.get("channel_mode") if experiment.get("channel_mode") not in {None, "unknown"}
        else ("RGB" if payload.get("input", {}).get("channels") == 3 else "RGBA"),
        "configuration_id": experiment.get("configuration_id", path.parent.name),
        "is_warmup": bool(experiment.get("is_warmup", False)),
        "measured_run": experiment.get("measured_run"),
        "width": width,
        "height": height,
        "pixels": int(width or 0) * int(height or 0),
        "channels": payload.get("input", {}).get("channels"),
        "threads": configuration.get("threads"),
        "processes": configuration.get("processes"),
        "blocks": configuration.get("blocks"),
        "segment_length": configuration.get("segment_length"),
        "cuda_threads_per_block": configuration.get("cuda_threads_per_block"),
        "output_bytes": output.get("bytes"),
        "compression_ratio": output.get("compression_ratio"),
        "throughput_mpixels": output.get("throughput_mpixels"),
        "core_pipeline_throughput_mpixels": output.get("core_pipeline_throughput_mpixels"),
        "validation_passed": validation.get("passed"),
        "pixel_match": validation.get("pixel_match"),
        "sha256_match": validation.get("sha256_match"),
        "process_wall_ms": experiment.get("process_wall_ms"),
        "request_roundtrip_ms": experiment.get("request_roundtrip_ms"),
        "worker_startup_ms": experiment.get("worker_startup_ms"),
        "worker_reused": experiment.get("worker_reused", False),
        "input_cache_reused": experiment.get("input_cache_reused", configuration.get("input_cache_reused", False)),
        "persistent_mpi": experiment.get("persistent_mpi", False),
        "inherited_index_hits": cross_block.get("inherited_index_hits", 0),
        "fallback_bytes_avoided": cross_block.get("fallback_bytes_avoided", 0),
    }
    row.update({field: timing.get(field, None if field == "core_pipeline_ms" else 0.0) for field in TIMING_FIELDS})
    # Keep a short pipeline alias in CSV reports while retaining the native
    # core_pipeline_* names used by the result contract.
    row["pipeline_ms"] = row["core_pipeline_ms"]
    row["pass1_ms"] = row["summary_ms"]
    row["pass2_ms"] = row["encode_ms"]
    row["communication_ms"] = row["transfer_in_ms"] + row["transfer_out_ms"]
    row.update({f"chunks_{field}": chunks.get(field, 0) for field in CHUNK_FIELDS})
    return row


def aggregate_image(group: list[dict[str, Any]]) -> dict[str, Any]:
    first = group[0]
    row = {key: first.get(key) for key in (
        "stage", "image_id", "category", "channel_mode", "backend", "configuration_id",
        "width", "height", "pixels", "channels", "threads", "processes", "blocks", "segment_length", "cuda_threads_per_block",
    )}
    row["measured_runs"] = len(group)
    row["all_valid"] = all(item.get("validation_passed") is True for item in group)
    for field in (*TIMING_FIELDS, *DERIVED_PHASE_FIELDS, "process_wall_ms"):
        row.update(describe((item.get(field) for item in group), field))
    row.update(describe((item.get("pipeline_ms") for item in group), "pipeline_ms"))
    for field in ("output_bytes", "compression_ratio", "throughput_mpixels", "core_pipeline_throughput_mpixels", "inherited_index_hits",
                  "fallback_bytes_avoided", *[f"chunks_{name}" for name in CHUNK_FIELDS]):
        row.update(describe((item.get(field) for item in group), field))
    return row


def worker_count(row: dict[str, Any]) -> float | None:
    if row.get("backend") == "openmp":
        return float(row.get("threads") or 1)
    if row.get("backend") == "mpi":
        return float(row.get("processes") or row.get("threads") or 1)
    if row.get("backend") in {"serial", "one-pass", "control"}:
        return 1.0
    return None


def add_derived_metrics(rows: list[dict[str, Any]]) -> None:
    by_image: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        key = (str(row.get("stage")), str(row["image_id"]))
        by_image[key].append(row)
    for image_rows in by_image.values():
        serial = next((row for row in image_rows if row.get("backend") == "serial"), None)
        serial_time = serial.get("encode_ms_median") if serial else None
        serial_pipeline_time = serial.get("core_pipeline_ms_median") if serial else None
        serial_bytes = serial.get("output_bytes_median") if serial else None
        for row in image_rows:
            encode = row.get("encode_ms_median")
            speedup = float(serial_time) / float(encode) if serial_time and encode else None
            pipeline = row.get("core_pipeline_ms_median")
            pipeline_speedup = (
                float(serial_pipeline_time) / float(pipeline)
                if serial_pipeline_time and pipeline else None
            )
            row["speedup"] = speedup
            workers = worker_count(row)
            row["efficiency"] = speedup / workers if speedup is not None and workers else None
            row["pipeline_speedup"] = pipeline_speedup
            row["pipeline_efficiency"] = pipeline_speedup / workers if pipeline_speedup is not None and workers else None
            row["pipeline_time_median"] = row.get("pipeline_ms_median")
            row["pipeline_time_stdev"] = row.get("pipeline_ms_stdev")
            output_bytes = row.get("output_bytes_median")
            row["size_overhead_percent"] = (
                (float(output_bytes) - float(serial_bytes)) / float(serial_bytes) * 100.0
                if output_bytes is not None and serial_bytes else None
            )


def group_measured_runs(rows: Iterable[dict[str, Any]]) -> dict[tuple[str, str, str, str], list[dict[str, Any]]]:
    """Keep stages and backends isolated when aggregating repeated runs."""
    groups: dict[tuple[str, str, str, str], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        key = (
            str(row.get("stage")),
            str(row.get("image_id")),
            str(row.get("backend")),
            str(row.get("configuration_id")),
        )
        groups[key].append(row)
    return groups


def aggregate_suite(rows: list[dict[str, Any]], keys: tuple[str, ...]) -> list[dict[str, Any]]:
    groups: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        groups[tuple(row.get(key) for key in keys)].append(row)
    result = []
    for key_values, group in sorted(groups.items(), key=lambda item: tuple(str(value) for value in item[0])):
        row = dict(zip(keys, key_values))
        total_pixels = sum(int(item.get("pixels") or 0) for item in group)
        total_encode_ms = sum(float(item.get("encode_ms_median") or 0.0) for item in group)
        pipeline_values = [item.get("core_pipeline_ms_median") for item in group]
        total_pipeline_ms = (
            sum(float(value) for value in pipeline_values)
            if all(value is not None and float(value) > 0.0 for value in pipeline_values)
            else None
        )
        row.update({
            "images": len(group),
            "total_pixels": total_pixels,
            "total_encode_ms": total_encode_ms,
            "suite_throughput_mpixels": total_pixels / (total_encode_ms * 1000.0) if total_encode_ms > 0 else None,
            "total_core_pipeline_ms": total_pipeline_ms,
            "total_pipeline_ms": total_pipeline_ms,
            "suite_core_pipeline_throughput_mpixels": (
                total_pixels / (total_pipeline_ms * 1000.0) if total_pipeline_ms and total_pipeline_ms > 0 else None
            ),
            "suite_pipeline_throughput_mpixels": (
                total_pixels / (total_pipeline_ms * 1000.0) if total_pipeline_ms and total_pipeline_ms > 0 else None
            ),
            "total_output_bytes": sum(float(item.get("output_bytes_median") or 0.0) for item in group),
            "all_valid": all(bool(item.get("all_valid")) for item in group),
        })
        for field in ("encode_ms_median", "core_pipeline_ms_median", "speedup", "efficiency", "pipeline_speedup",
                      "pipeline_efficiency", "size_overhead_percent", "compression_ratio_median"):
            row.update(describe((item.get(field) for item in group), field))
        result.append(row)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--input-dir", type=Path)
    source.add_argument("--per-run-input", type=Path,
                        help="Existing flattened per-run CSV; avoids rescanning all result JSON files")
    parser.add_argument("--output", type=Path, required=True, help="Per-run CSV path")
    parser.add_argument("--summary-dir", type=Path)
    args = parser.parse_args()
    summary_dir = args.summary_dir or args.output.parent

    if args.per_run_input:
        per_run = read_per_run_csv(args.per_run_input)
        if args.per_run_input.resolve() != args.output.resolve():
            write_csv(args.output, per_run)
    else:
        per_run = []
        for path in sorted(args.input_dir.rglob("*.json")):
            try:
                payload = json.loads(path.read_text(encoding="utf-8"))
            except (json.JSONDecodeError, OSError):
                continue
            if "timing" in payload:
                per_run.append(flatten(path, payload))
        write_csv(args.output, per_run)

    measured = [row for row in per_run if not row["is_warmup"] and row["status"] == "success"]
    groups = group_measured_runs(measured)
    per_image = [aggregate_image(group) for group in groups.values()]
    add_derived_metrics(per_image)
    per_image.sort(key=lambda row: (
        str(row.get("stage")), str(row["image_id"]), str(row["backend"]), str(row["configuration_id"]),
    ))
    write_csv(summary_dir / "per-image-summary.csv", per_image)
    write_csv(summary_dir / "category-summary.csv", aggregate_suite(per_image, ("stage", "category", "backend", "configuration_id")))
    write_csv(summary_dir / "full-suite-summary.csv", aggregate_suite(per_image, ("stage", "backend", "configuration_id")))
    scalability_fields = (
        "stage", "image_id", "category", "backend", "configuration_id", "pixels", "width", "height",
        "threads", "processes", "blocks", "segment_length", "cuda_threads_per_block", "encode_ms_median", "encode_ms_mean",
        "encode_ms_stdev", "speedup", "efficiency", "throughput_mpixels_median",
        "core_pipeline_ms_median", "core_pipeline_throughput_mpixels_median", "pipeline_speedup", "pipeline_efficiency",
        "pipeline_ms_median", "pipeline_ms_stdev", "pipeline_time_median", "pipeline_time_stdev",
    )
    write_csv(summary_dir / "scalability-summary.csv", [
        {field: row.get(field) for field in scalability_fields} for row in per_image
    ])
    print(f"aggregated {len(measured)} measured runs into {len(per_image)} per-image configurations")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
