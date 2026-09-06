from __future__ import annotations

import importlib.util
import csv
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def load_script(name: str):
    path = REPO_ROOT / "benchmark" / "scripts" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


aggregate_results = load_script("aggregate_results")
create_excel_report = load_script("create_excel_report")


class ReportingPipelineTests(unittest.TestCase):
    def test_repeated_runs_are_isolated_by_stage_and_backend(self) -> None:
        rows = []
        for stage in ("tuning", "full"):
            for backend in ("serial", "openmp"):
                for run in range(5):
                    rows.append({
                        "stage": stage,
                        "image_id": "shared-image",
                        "backend": backend,
                        "configuration_id": "serial_blo-1" if backend == "serial" else "openmp_thr-4_blo-8",
                        "run": run + 1,
                    })

        groups = aggregate_results.group_measured_runs(rows)

        self.assertEqual(4, len(groups))
        self.assertTrue(all(len(group) == 5 for group in groups.values()))

    def test_derived_metrics_use_serial_from_the_same_stage(self) -> None:
        rows = [
            {"stage": "tuning", "image_id": "shared", "backend": "serial", "encode_ms_median": 100.0, "output_bytes_median": 1000.0, "bmp_bytes_median": 4000.0},
            {"stage": "tuning", "image_id": "shared", "backend": "openmp", "threads": 4, "encode_ms_median": 25.0, "output_bytes_median": 1010.0, "bmp_bytes_median": 4000.0},
            {"stage": "full", "image_id": "shared", "backend": "serial", "encode_ms_median": 10.0, "output_bytes_median": 2000.0, "bmp_bytes_median": 8000.0},
            {"stage": "full", "image_id": "shared", "backend": "openmp", "threads": 4, "encode_ms_median": 5.0, "output_bytes_median": 2020.0, "bmp_bytes_median": 8000.0},
        ]

        aggregate_results.add_derived_metrics(rows)

        tuning_openmp = rows[1]
        full_openmp = rows[3]
        self.assertEqual(4.0, tuning_openmp["speedup"])
        self.assertEqual(2.0, full_openmp["speedup"])
        self.assertEqual(1.0, tuning_openmp["efficiency"])
        self.assertEqual(0.5, full_openmp["efficiency"])
        self.assertAlmostEqual(1.0, tuning_openmp["size_overhead_percent"])
        self.assertAlmostEqual(1.0, full_openmp["size_overhead_percent"])
        self.assertAlmostEqual(4000.0 / 1010.0, tuning_openmp["compression_ratio"])
        self.assertAlmostEqual((1.0 - 1010.0 / 4000.0) * 100.0, tuning_openmp["size_reduction_percent"])

    def test_summary_directory_resolution_supports_current_layout(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            summary = root / "summary"
            tuning = root / "tuning-summary"
            summary.mkdir()
            tuning.mkdir()
            (summary / "full-suite-summary.csv").write_text("stage\nfull\n", encoding="utf-8")

            resolved_summary, resolved_tuning = create_excel_report.resolve_summary_dirs(summary)

            self.assertEqual(summary, resolved_summary)
            self.assertEqual(tuning, resolved_tuning)

    def test_per_run_csv_restores_boolean_fields_and_bmp_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "per-run.csv"
            with path.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=[
                    "is_warmup", "validation_passed", "pixel_match", "width", "height", "output_bytes",
                ])
                writer.writeheader()
                writer.writerow({
                    "is_warmup": "False",
                    "validation_passed": "True",
                    "pixel_match": "True",
                    "width": "8",
                    "height": "4",
                    "output_bytes": "100",
                })

            row = aggregate_results.read_per_run_csv(path)[0]

            self.assertIs(row["is_warmup"], False)
            self.assertIs(row["validation_passed"], True)
            self.assertIs(row["pixel_match"], True)
            self.assertEqual(54 + 8 * 4 * 4, row["bmp_bytes"])
            self.assertAlmostEqual(row["bmp_bytes"] / 100.0, row["compression_ratio"])

    def test_phase_summary_is_normalized_without_input_load_or_validation(self) -> None:
        rows = [{
            "stage": "full", "backend": "serial", "configuration_id": "serial_blo-1",
            "all_valid": "true", "cuda_init_ms_median": 1.0, "openmp_init_ms_median": 2.0, "allocation_ms_median": 1.0,
            "summary_ms_median": 2.0, "propagation_ms_median": 1.0,
            "transfer_in_ms_median": 1.0, "encode_ms_median": 2.0,
            "prefix_scan_ms_median": 1.0, "compaction_ms_median": 1.0,
            "transfer_out_ms_median": 1.0, "merge_ms_median": 1.0,
            "load_ms_median": 100.0, "validation_ms_median": 100.0,
        }]

        summary = create_excel_report.phase_summary(rows, "full")[0]

        self.assertAlmostEqual(100.0, sum(summary[field.replace("_median", "")] for field, _label in create_excel_report.PHASE_COLUMNS))
        self.assertAlmostEqual(2.0 / 14.0 * 100.0, summary["encode_ms"])
        self.assertAlmostEqual(2.0 / 14.0 * 100.0, summary["openmp_init_ms"])

    def test_process_wall_time_is_retained_in_per_image_summary(self) -> None:
        group = [{
            "stage": "full",
            "image_id": "image",
            "backend": "serial",
            "configuration_id": "serial_blo-1",
            "validation_passed": True,
            "process_wall_ms": value,
        } for value in (10.0, 20.0, 30.0, 40.0, 50.0)]

        summary = aggregate_results.aggregate_image(group)

        self.assertEqual(30.0, summary["process_wall_ms_median"])


if __name__ == "__main__":
    unittest.main()
