"""Create a report-ready Excel workbook from aggregated benchmark CSV files.

The workbook is intentionally a snapshot of the benchmark summaries.  It keeps
the source aggregates in filterable sheets and adds Excel chart objects for the
metrics normally used in the project report.
"""

from __future__ import annotations

import argparse
import csv
import math
import re
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable

import xlsxwriter


BACKENDS = ("serial", "openmp", "cuda", "mpi", "one-pass")
BACKEND_LABELS = {
    "serial": "Serial",
    "openmp": "OpenMP",
    "cuda": "CUDA",
    "mpi": "MPI",
    "one-pass": "One-pass control",
}
BACKEND_COLORS = {
    "serial": "#4B5563",
    "openmp": "#2563EB",
    "cuda": "#F97316",
    "mpi": "#16A34A",
    "one-pass": "#9333EA",
}

PHASE_COLUMNS = (
    ("load_ms_median", "Input decode"),
    ("cuda_init_ms_median", "CUDA init"),
    ("allocation_ms_median", "GPU allocation"),
    ("summary_ms_median", "Pass 1 / summary"),
    ("propagation_ms_median", "Propagation"),
    ("transfer_in_ms_median", "Transfer in"),
    ("encode_ms_median", "Pass 2 / encode"),
    ("transfer_out_ms_median", "Transfer out"),
    ("compaction_ms_median", "Compaction"),
    ("merge_ms_median", "Merge"),
    ("validation_ms_median", "Validation"),
)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open("r", newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def number(value: Any, default: float | None = None) -> float | None:
    if value in (None, "", "None", "null"):
        return default
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return default
    return parsed if math.isfinite(parsed) else default


def integer(value: Any, default: int | None = None) -> int | None:
    parsed = number(value)
    return int(parsed) if parsed is not None else default


def median(values: Iterable[Any]) -> float:
    numbers = [parsed for value in values if (parsed := number(value)) is not None]
    return statistics.median(numbers) if numbers else 0.0


def display_backend(backend: str) -> str:
    return BACKEND_LABELS.get(backend, backend)


def resolve_summary_dirs(results_dir: Path) -> tuple[Path, Path]:
    """Resolve consolidated report summaries while retaining legacy support."""
    if (results_dir / "full-suite-summary.csv").exists():
        return results_dir, results_dir.parent / "tuning-summary"
    if (results_dir / "summary" / "full-suite-summary.csv").exists():
        return results_dir / "summary", results_dir / "tuning-summary"
    legacy_full = results_dir / "full" / "summary"
    legacy_tuning = results_dir / "tuning" / "summary"
    if (legacy_full / "full-suite-summary.csv").exists():
        return legacy_full, legacy_tuning
    return results_dir, results_dir.parent / "tuning-summary"


def parse_config(config: str, backend: str) -> tuple[float | None, int | None]:
    """Return the primary tuning parameter and optional partition count."""
    if backend == "openmp":
        match = re.search(r"thr-(\d+)_blo-(\d+)", config)
        return (float(match.group(1)), int(match.group(2))) if match else (None, None)
    if backend == "cuda":
        match = re.search(r"seg-(\d+)", config)
        return (float(match.group(1)), None) if match else (None, None)
    if backend == "mpi":
        match = re.search(r"pro-(\d+)_blo-(\d+)", config)
        return (float(match.group(1)), int(match.group(2))) if match else (None, None)
    match = re.search(r"blo-(\d+)", config)
    return (float(match.group(1)), int(match.group(1))) if match else (None, None)


def phase_summary(per_image_rows: list[dict[str, str]], stage: str) -> list[dict[str, Any]]:
    grouped: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in per_image_rows:
        if row.get("stage") == stage:
            grouped[row.get("backend", "unknown")].append(row)
    result: list[dict[str, Any]] = []
    for backend in BACKENDS:
        rows = grouped.get(backend, [])
        if not rows:
            continue
        output: dict[str, Any] = {
            "stage": stage,
            "backend": display_backend(backend),
            "configuration": rows[0].get("configuration_id", ""),
            "images": len(rows),
            "all_valid": all(row.get("all_valid", "").lower() == "true" for row in rows),
        }
        for field, _label in PHASE_COLUMNS:
            output[field.replace("_median", "")] = median(row.get(field, "0") for row in rows)
        result.append(output)
    return result


def scalability_bins(rows: list[dict[str, str]], stage: str) -> list[dict[str, Any]]:
    """Aggregate per-image rows into readable log-size bins for a report chart."""
    grouped: dict[tuple[str, int], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        if row.get("stage") != stage:
            continue
        pixels = number(row.get("pixels"))
        if pixels is None or pixels <= 0:
            continue
        bucket = int(math.floor(math.log10(pixels)))
        grouped[(row.get("backend", "unknown"), bucket)].append(row)
    result: list[dict[str, Any]] = []
    for (backend, bucket), bucket_rows in sorted(grouped.items(), key=lambda item: (item[0][0], item[0][1])):
        result.append({
            "stage": stage,
            "backend": display_backend(backend),
            "pixel_bin": f"10^{bucket}–10^{bucket + 1}",
            "median_pixels": median(row.get("pixels") for row in bucket_rows),
            "encode_ms_median": median(row.get("encode_ms_median") for row in bucket_rows),
            "throughput_mpixels_median": median(row.get("throughput_mpixels_median") for row in bucket_rows),
            "core_pipeline_ms_median": median(row.get("core_pipeline_ms_median") for row in bucket_rows),
            "core_pipeline_throughput_mpixels_median": median(row.get("core_pipeline_throughput_mpixels_median") for row in bucket_rows),
            "images": len(bucket_rows),
        })
    return result


def speedup_scatter_rows(per_image_rows: list[dict[str, str]]) -> list[list[Any]]:
    """Align per-image speedups for the four user-facing backends."""
    grouped: dict[str, dict[str, str]] = {}
    for row in per_image_rows:
        if row.get("stage") != "full" or row.get("backend") not in {"serial", "openmp", "cuda", "mpi"}:
            continue
        image_id = row.get("image_id", "")
        grouped.setdefault(image_id, {})
        grouped[image_id]["category"] = row.get("category", "")
        grouped[image_id]["pixels"] = row.get("pixels", "")
        backend = row.get("backend", "")
        grouped[image_id][backend] = "1.0" if backend == "serial" else row.get("speedup", "")

    rows: list[list[Any]] = []
    for image_id, values in sorted(grouped.items(), key=lambda item: (number(item[1].get("pixels"), 0), item[0])):
        pixels = number(values.get("pixels"))
        if pixels is None or pixels <= 0:
            continue
        rows.append([
            image_id,
            values.get("category", ""),
            pixels,
            number(values.get("serial")),
            number(values.get("cuda")),
            number(values.get("openmp")),
            number(values.get("mpi")),
        ])
    return rows


def write_table(worksheet: xlsxwriter.worksheet.Worksheet, start_row: int, start_col: int,
                headers: list[str], rows: list[list[Any]], formats: dict[str, xlsxwriter.format.Format],
                widths: list[float] | None = None) -> tuple[int, int]:
    for col, header in enumerate(headers):
        worksheet.write(start_row, start_col + col, header, formats["header"])
        if widths and col < len(widths):
            worksheet.set_column(start_col + col, start_col + col, widths[col])
    for row_index, row in enumerate(rows, start_row + 1):
        for col_index, value in enumerate(row):
            header = headers[col_index] if col_index < len(headers) else ""
            if "ms" in header.lower():
                value_format = formats["ms"]
            elif "mpix/s" in header.lower():
                value_format = formats["mpix"]
            elif "speedup" in header.lower() or "ratio" in header.lower():
                value_format = formats["ratio"]
            elif "overhead" in header.lower():
                value_format = formats["percent_points"]
            elif "efficiency" in header.lower():
                value_format = formats["percent"]
            elif any(token in header.lower() for token in ("images", "pixels", "bytes", "blocks")):
                value_format = formats["integer"]
            else:
                value_format = formats["number"]
            if isinstance(value, bool):
                worksheet.write_boolean(row_index, start_col + col_index, value, formats["bool"])
            elif isinstance(value, (int, float)) and not isinstance(value, bool):
                worksheet.write_number(row_index, start_col + col_index, value, value_format)
            else:
                worksheet.write(row_index, start_col + col_index, value if value is not None else "", formats["text"])
    if rows:
        worksheet.add_table(
            start_row, start_col, start_row + len(rows), start_col + len(headers) - 1,
            {"columns": [{"header": header} for header in headers], "style": "Table Style Medium 2"},
        )
    return start_row + len(rows), start_col + len(headers)


def write_summary_sheet(workbook: xlsxwriter.Workbook, name: str, title: str, note: str,
                       headers: list[str], rows: list[list[Any]], formats: dict[str, xlsxwriter.format.Format],
                       widths: list[float] | None = None) -> xlsxwriter.worksheet.Worksheet:
    worksheet = workbook.add_worksheet(name)
    worksheet.hide_gridlines(2)
    worksheet.write("A1", title, formats["title"])
    worksheet.write("A2", note, formats["note"])
    worksheet.merge_range(1, 0, 1, max(0, len(headers) - 1), note, formats["note"])
    write_table(worksheet, 3, 0, headers, rows, formats, widths)
    worksheet.freeze_panes(4, 0)
    return worksheet


def chart_title(chart: xlsxwriter.chart.Chart, title: str) -> None:
    chart.set_title({"name": title})
    chart.set_legend({"position": "bottom"})
    chart.set_chartarea({"border": {"none": True}, "fill": {"color": "#FFFFFF"}})
    chart.set_plotarea({"border": {"color": "#D9E0E6"}, "fill": {"color": "#FBFCFD"}})


def add_backend_column_chart(workbook: xlsxwriter.Workbook, sheet_name: str, row_count: int,
                             category_col: int, value_col: int, title: str, y_name: str,
                             source_row: int = 3) -> xlsxwriter.chart.Chart:
    chart = workbook.add_chart({"type": "column"})
    for row in range(source_row + 1, source_row + row_count + 1):
        backend = None
        # Color is applied per point below through a point index; the series itself
        # remains a single series so the legend stays compact.
        _ = backend
    points = [{"fill": {"color": BACKEND_COLORS.get("serial", "#9AA6B2")}}]
    chart.add_series({
        "name": title,
        "categories": [sheet_name, source_row + 1, category_col, source_row + row_count, category_col],
        "values": [sheet_name, source_row + 1, value_col, source_row + row_count, value_col],
        "fill": {"color": "#7E9E24"},
        "border": {"none": True},
        "points": points,
    })
    chart.set_x_axis({"name": "Backend", "label_position": "low"})
    chart.set_y_axis({"name": y_name, "major_gridlines": {"visible": False}})
    chart_title(chart, title)
    return chart


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results-dir", type=Path, default=Path("results/evaluation"))
    parser.add_argument("--output", type=Path, default=Path("results/evaluation/parallel-qoi-benchmark-report.xlsx"))
    args = parser.parse_args()

    final_summary_dir, tuning_summary_dir = resolve_summary_dirs(args.results_dir)
    full_suite = [
        row for row in read_csv(final_summary_dir / "full-suite-summary.csv")
        if row.get("stage") == "full"
    ]
    category_summary = [
        row for row in read_csv(final_summary_dir / "category-summary.csv")
        if row.get("stage") == "full"
    ]
    all_per_image = read_csv(final_summary_dir / "per-image-summary.csv")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    workbook = xlsxwriter.Workbook(args.output)
    workbook.set_properties({
        "title": "Parallel QOI Benchmark Report",
        "subject": "Serial, OpenMP, CUDA and MPI performance comparison",
        "author": "Parallel QOI Converter",
        "comments": "Generated from results/evaluation summary CSV files.",
    })

    formats = {
        "title": workbook.add_format({"bold": True, "font_size": 20, "font_color": "#26313B"}),
        "subtitle": workbook.add_format({"font_size": 11, "font_color": "#6B7885"}),
        "note": workbook.add_format({"font_size": 10, "font_color": "#6B7885", "text_wrap": True, "valign": "top"}),
        "section": workbook.add_format({"bold": True, "font_size": 12, "font_color": "#587616", "top": 1, "top_color": "#D9E0E6"}),
        "header": workbook.add_format({"bold": True, "font_color": "#FFFFFF", "bg_color": "#607F16", "border": 0, "text_wrap": True, "valign": "vcenter"}),
        "text": workbook.add_format({"font_color": "#26313B"}),
        "number": workbook.add_format({"font_color": "#26313B", "num_format": "0.00"}),
        "integer": workbook.add_format({"font_color": "#26313B", "num_format": "#,##0"}),
        "ms": workbook.add_format({"font_color": "#26313B", "num_format": "0.00 \"ms\""}),
        "mpix": workbook.add_format({"font_color": "#26313B", "num_format": "0.00 \"MPix/s\""}),
        "percent": workbook.add_format({"font_color": "#26313B", "num_format": "0.00%"}),
        "percent_points": workbook.add_format({"font_color": "#26313B", "num_format": "0.00\"%\""}),
        "ratio": workbook.add_format({"font_color": "#26313B", "num_format": "0.00\"×\""}),
        "bool": workbook.add_format({"font_color": "#26313B", "align": "center"}),
        "kpi_label": workbook.add_format({"bold": True, "font_color": "#6B7885", "bg_color": "#F2F5F7"}),
        "kpi_value": workbook.add_format({"bold": True, "font_size": 14, "font_color": "#587616", "bg_color": "#F2F5F7", "num_format": "0.00"}),
    }

    # Report dashboard.
    report = workbook.add_worksheet("Report")
    report.hide_gridlines(2)
    report.set_column("A:A", 3)
    report.set_column("B:N", 14)
    report.write("B2", "Parallel QOI Benchmark Report", formats["title"])
    report.write("B3", "Excel charts generated from aggregated benchmark results", formats["subtitle"])
    report.merge_range("B4:N4", "Primary benchmark statistic: per-image median over five measured runs after one warm-up. Validation is shown separately from performance.", formats["note"])

    full_rows = [row for row in full_suite if row.get("backend") in BACKENDS]
    full_rows.sort(key=lambda row: BACKENDS.index(row.get("backend", "one-pass")))
    total_images = integer(full_rows[0].get("images")) if full_rows else 0
    total_pixels = integer(full_rows[0].get("total_pixels")) if full_rows else 0
    report.write("B6", "Scope", formats["section"])
    kpis = [("Stage", "Full"), ("Images", total_images), ("Total pixels", total_pixels), ("Backends", len(full_rows))]
    for index, (label, value) in enumerate(kpis):
        col = 1 + index * 3
        report.write(7, col, label, formats["kpi_label"])
        if isinstance(value, (int, float)):
            report.write_number(8, col, value, formats["kpi_value"])
        else:
            report.write(8, col, value, formats["kpi_value"])
        report.set_column(1 + index * 3, 2 + index * 3, 14)

    overview_headers = ["Backend", "Encode median (ms)", "Core pipeline median (ms)", "Encode suite throughput (MPix/s)", "Core pipeline suite throughput (MPix/s)", "Speedup (×)", "Efficiency", "Output bytes", "Compression ratio", "Size overhead (%)", "All valid"]
    overview_rows: list[list[Any]] = []
    for row in full_rows:
        overview_rows.append([
            display_backend(row.get("backend", "")),
            number(row.get("encode_ms_median_median"), 0),
            number(row.get("core_pipeline_ms_median_median"), 0),
            number(row.get("suite_throughput_mpixels"), 0),
            number(row.get("suite_core_pipeline_throughput_mpixels"), 0),
            number(row.get("speedup_median"), 0),
            number(row.get("efficiency_median")),
            number(row.get("total_output_bytes"), 0),
            number(row.get("compression_ratio_median_median"), 0),
            number(row.get("size_overhead_percent_median")),
            row.get("all_valid", "").lower() == "true",
        ])
    overview = write_summary_sheet(
        workbook, "Full Suite", "Full-suite backend summary",
        "Source: results/evaluation/full/summary/full-suite-summary.csv. One-pass control is retained for the research comparison but is not a product backend.",
        overview_headers, overview_rows, formats, [20, 18, 24, 26, 32, 14, 14, 18, 20, 20, 12],
    )
    overview.set_column(1, 1, 18, formats["ms"])
    overview.set_column(2, 2, 24, formats["ms"])
    overview.set_column(3, 4, 30, formats["mpix"])
    overview.set_column(5, 5, 14, formats["ratio"])
    overview.set_column(6, 6, 14, formats["percent"])
    overview.set_column(7, 7, 18, formats["integer"])
    overview.set_column(8, 8, 20, formats["number"])
    overview.set_column(9, 9, 20, formats["percent_points"])

    # Add report charts using the compact full-suite table on the Full Suite sheet.
    first_data_row = 4
    chart_specs = [
        ("Encode median by backend", 1, "Encode (ms)"),
        ("Core pipeline median by backend", 2, "Core pipeline (ms)"),
        ("Effective core pipeline throughput", 4, "MPix/s"),
        ("Compression ratio", 8, "Raw bytes / QOI bytes"),
    ]
    chart_positions = ["B11", "J11", "B28", "J28"]
    for (title, value_col, y_label), position in zip(chart_specs, chart_positions):
        chart = workbook.add_chart({"type": "column"})
        chart.add_series({
            "name": title,
            "categories": ["Full Suite", first_data_row, 0, first_data_row + len(overview_rows) - 1, 0],
            "values": ["Full Suite", first_data_row, value_col, first_data_row + len(overview_rows) - 1, value_col],
            "fill": {"color": "#7E9E24"},
            "border": {"none": True},
            "points": [{"fill": {"color": BACKEND_COLORS.get(row.get("backend", ""), "#7E9E24")}, "border": {"none": True}} for row in full_rows],
        })
        chart.set_size({"width": 560, "height": 300})
        chart.set_x_axis({"name": "Backend"})
        chart.set_y_axis({"name": y_label, "major_gridlines": {"visible": False}})
        chart_title(chart, title)
        report.insert_chart(position, chart)

    # Category data and chart.
    category_headers = ["Category", "Backend", "Configuration", "Images", "Encode median (ms)", "Core pipeline median (ms)", "Speedup (×)", "Efficiency", "Encode throughput (MPix/s)", "Core pipeline throughput (MPix/s)", "Output bytes", "Compression ratio", "Size overhead (%)", "All valid"]
    category_rows: list[list[Any]] = []
    for row in category_summary:
        category_rows.append([
            row.get("category", ""), display_backend(row.get("backend", "")), row.get("configuration_id", ""),
            integer(row.get("images"), 0), number(row.get("encode_ms_median_median"), 0),
            number(row.get("core_pipeline_ms_median_median"), 0), number(row.get("speedup_median"), 0),
            number(row.get("efficiency_median")), number(row.get("suite_throughput_mpixels"), 0),
            number(row.get("suite_core_pipeline_throughput_mpixels"), 0), number(row.get("total_output_bytes"), 0),
            number(row.get("compression_ratio_median_median"), 0), number(row.get("size_overhead_percent_median")),
            row.get("all_valid", "").lower() == "true",
        ])
    category_sheet = write_summary_sheet(
        workbook, "Category Summary", "Full-stage category comparison",
        "Grouped by image category. Use the filters to isolate a backend or category before copying a chart into the report.",
        category_headers, category_rows, formats, [24, 18, 24, 10, 18, 24, 14, 14, 24, 30, 18, 20, 20, 12],
    )
    category_sheet.set_column(4, 4, 18, formats["ms"])
    category_sheet.set_column(5, 5, 24, formats["ms"])
    category_sheet.set_column(6, 6, 14, formats["ratio"])
    category_sheet.set_column(7, 7, 14, formats["percent"])
    category_sheet.set_column(8, 9, 26, formats["mpix"])
    category_sheet.set_column(10, 10, 18, formats["integer"])
    category_sheet.set_column(11, 11, 20, formats["number"])
    category_sheet.set_column(12, 12, 20, formats["percent_points"])

    # Compact category matrix used by a report-ready grouped speedup chart.
    category_names = sorted({row.get("category", "") for row in category_summary if row.get("category")})
    category_lookup = {(row.get("category", ""), row.get("backend", "")): row for row in category_summary}
    category_plot_headers = ["Category"] + [display_backend(backend) for backend in BACKENDS]
    category_plot_rows: list[list[Any]] = []
    for category in category_names:
        category_plot_rows.append([
            category,
            *[number(category_lookup.get((category, backend), {}).get("speedup_median")) for backend in BACKENDS],
        ])
    category_plot_sheet = write_summary_sheet(
        workbook, "Category Plot Data", "Category speedup plot data",
        "Speedup median by image category. Blank cells mean that a backend/configuration was not present for that category.",
        category_plot_headers, category_plot_rows, formats, [30] + [18] * len(BACKENDS),
    )
    for column in range(1, 1 + len(BACKENDS)):
        category_plot_sheet.set_column(column, column, 18, formats["ratio"])
    if category_plot_rows:
        category_chart = workbook.add_chart({"type": "column"})
        for column, backend in enumerate(BACKENDS, 1):
            category_chart.add_series({
                "name": ["Category Plot Data", 3, column],
                "categories": ["Category Plot Data", 4, 0, 3 + len(category_plot_rows), 0],
                "values": ["Category Plot Data", 4, column, 3 + len(category_plot_rows), column],
                "fill": {"color": BACKEND_COLORS[backend]},
                "border": {"none": True},
            })
        category_chart.set_size({"width": 850, "height": 380})
        category_chart.set_x_axis({"name": "Image category", "label_position": "low", "num_font": {"rotation": -35}})
        category_chart.set_y_axis({"name": "Speedup (×)", "major_gridlines": {"visible": False}})
        chart_title(category_chart, "Speedup by image category")
        report.insert_chart("B67", category_chart)

    # Per-image speedup scatter plot. Each point is one Full-stage image and each
    # accelerated backend is normalized against that image's Serial encode time.
    # Serial is drawn as a neutral 1.0x reference line rather than a trendline.
    scatter_headers = ["Image ID", "Category", "Input pixels", "Serial baseline (x)", "CUDA speedup (x)", "OpenMP speedup (x)", "MPI speedup (x)"]
    scatter_rows = speedup_scatter_rows(all_per_image)
    scatter_sheet = write_summary_sheet(
        workbook, "Speedup Scatter Data", "Per-image speedup scatter data",
        "One point per Full-stage image. Speedup is Serial encode time divided by backend encode time; values above 1.0x are faster than Serial. Actual encode times remain available in Report and Full Suite. The X axis is logarithmic to cover small and large images clearly.",
        scatter_headers, scatter_rows, formats, [30, 24, 18, 22, 20, 22, 20],
    )
    scatter_sheet.set_column(2, 2, 18, formats["integer"])
    scatter_sheet.set_column(3, 6, 20, formats["ratio"])
    if scatter_rows:
        scatter_chart = workbook.add_chart({"type": "scatter", "subtype": "markers_only"})
        scatter_chart.add_series({
            "name": ["Speedup Scatter Data", 3, 3],
            "categories": ["Speedup Scatter Data", 4, 2, 3 + len(scatter_rows), 2],
            "values": ["Speedup Scatter Data", 4, 3, 3 + len(scatter_rows), 3],
            "marker": {"type": "none"},
            "line": {"color": BACKEND_COLORS["serial"], "width": 1.75},
        })
        for column, backend in ((4, "cuda"), (5, "openmp"), (6, "mpi")):
            color = BACKEND_COLORS[backend]
            scatter_chart.add_series({
                "name": ["Speedup Scatter Data", 3, column],
                "categories": ["Speedup Scatter Data", 4, 2, 3 + len(scatter_rows), 2],
                "values": ["Speedup Scatter Data", 4, column, 3 + len(scatter_rows), column],
                "marker": {"type": "circle", "size": 3, "border": {"color": color}, "fill": {"color": color}},
                "line": {"none": True},
                "trendline": {"type": "linear", "line": {"color": color, "width": 1.5, "dash_type": "dash"}},
            })
        scatter_chart.set_size({"width": 930, "height": 430})
        scatter_chart.set_x_axis({"name": "Input pixels (log scale)", "log_base": 10, "major_gridlines": {"visible": False}})
        scatter_chart.set_y_axis({"name": "Speedup versus Serial (x)", "min": 0, "major_gridlines": {"visible": True}})
        chart_title(scatter_chart, "Per-image speedup versus input size")
        report.insert_chart("B88", scatter_chart)

    # Phase summaries are derived from the already aggregated per-image CSV.
    phase_headers = ["Stage", "Backend", "Configuration", "Images", "All valid"] + [label + " (ms)" for _field, label in PHASE_COLUMNS]
    phase_rows: list[list[Any]] = []
    for stage in ("correctness", "tuning", "full"):
        for row in phase_summary(all_per_image, stage):
            phase_rows.append([row["stage"], row["backend"], row["configuration"], row["images"], row["all_valid"]] + [row[field.replace("_median", "")] for field, _label in PHASE_COLUMNS])
    phase_sheet = write_summary_sheet(
        workbook, "Phase Summary", "Median phase timing by stage and backend",
        "Phase values are medians across the per-image medians. Missing CUDA setup fields in older artifacts are shown as 0; regenerate the benchmark after the latest native timing changes for final reporting.",
        phase_headers, phase_rows, formats, [14, 18, 24, 10, 12] + [17] * len(PHASE_COLUMNS),
    )
    phase_sheet.set_column(5, 4 + len(PHASE_COLUMNS), 17, formats["ms"])

    # Stacked phase chart for the full stage.
    full_phase_rows = [row for row in phase_rows if row[0] == "full"]
    if full_phase_rows:
        full_phase_start = 4
        phase_chart = workbook.add_chart({"type": "bar", "subtype": "stacked"})
        for index, (_field, label) in enumerate(PHASE_COLUMNS, 5):
            phase_chart.add_series({
                "name": ["Phase Summary", full_phase_start - 1, index],
                "categories": ["Phase Summary", full_phase_start, 1, full_phase_start + len(full_phase_rows) - 1, 1],
                "values": ["Phase Summary", full_phase_start, index, full_phase_start + len(full_phase_rows) - 1, index],
                "fill": {"color": ["#AEB7C1", "#D4DBE1", "#C4CCD4", "#9BA7B2", "#8997A4", "#B9C98E", "#789A22", "#94AD4D", "#B2BBC4", "#D8DEE4", "#C8D0D8"][index - 5]},
                "border": {"none": True},
            })
        phase_chart.set_size({"width": 850, "height": 380})
        phase_chart.set_x_axis({"name": "Duration (ms)", "major_gridlines": {"visible": False}})
        phase_chart.set_y_axis({"name": "Backend"})
        chart_title(phase_chart, "Full-suite median phase breakdown")
        report.insert_chart("B45", phase_chart)

    # Tuning data (one row per tested configuration) and a compact encode chart.
    tuning_suite = [
        row for row in read_csv(tuning_summary_dir / "full-suite-summary.csv")
        if row.get("stage") == "tuning"
    ]
    tuning_headers = ["Backend", "Configuration", "Primary parameter", "Blocks", "Images", "Core pipeline median (ms)", "Core pipeline throughput (MPix/s)", "Encode median (ms)", "Encode throughput (MPix/s)", "Speedup (×)", "Efficiency", "Compression ratio"]
    tuning_rows: list[list[Any]] = []
    for row in tuning_suite:
        backend = row.get("backend", "")
        primary, blocks = parse_config(row.get("configuration_id", ""), backend)
        tuning_rows.append([
            display_backend(backend), row.get("configuration_id", ""), primary, blocks, integer(row.get("images"), 0),
            number(row.get("core_pipeline_ms_median_median"), 0), number(row.get("suite_core_pipeline_throughput_mpixels"), 0),
            number(row.get("encode_ms_median_median"), 0), number(row.get("suite_throughput_mpixels"), 0),
            number(row.get("speedup_median"), 0), number(row.get("efficiency_median")),
            number(row.get("compression_ratio_median_median"), 0),
        ])
    tuning_sheet = write_summary_sheet(
        workbook, "Tuning Summary", "Tuning configuration results",
        "Primary parameter means threads for OpenMP, segment length for CUDA, processes for MPI, and block count for the control. Use this sheet to choose the configuration used in the full suite.",
        tuning_headers, tuning_rows, formats, [18, 24, 18, 10, 10, 24, 30, 18, 24, 14, 14, 20],
    )
    tuning_sheet.set_column(5, 5, 24, formats["ms"])
    tuning_sheet.set_column(6, 6, 30, formats["mpix"])
    tuning_sheet.set_column(7, 7, 18, formats["ms"])
    tuning_sheet.set_column(8, 8, 24, formats["mpix"])
    tuning_sheet.set_column(9, 9, 14, formats["ratio"])
    tuning_sheet.set_column(10, 10, 14, formats["percent"])
    tuning_sheet.set_column(11, 11, 20, formats["number"])

    # Size scalability data and chart.
    scale_headers = ["Stage", "Backend", "Pixel bin", "Median pixels", "Core pipeline median (ms)", "Core pipeline throughput (MPix/s)", "Encode median (ms)", "Encode throughput (MPix/s)", "Images"]
    scale_rows: list[list[Any]] = []
    for stage in ("correctness", "tuning", "full"):
        for row in scalability_bins(all_per_image, stage):
            scale_rows.append([row["stage"], row["backend"], row["pixel_bin"], row["median_pixels"], row["core_pipeline_ms_median"], row["core_pipeline_throughput_mpixels_median"], row["encode_ms_median"], row["throughput_mpixels_median"], row["images"]])
    scale_sheet = write_summary_sheet(
        workbook, "Scalability", "Image-size scalability summary",
        "Rows are log-size bins built from per-image summaries. This is intended for report plots; the complete per-image scalability CSV remains in results/evaluation.",
        scale_headers, scale_rows, formats, [14, 18, 18, 18, 24, 30, 20, 24, 10],
    )
    scale_sheet.set_column(3, 3, 18, formats["integer"])
    scale_sheet.set_column(4, 4, 20, formats["ms"])
    scale_sheet.set_column(5, 5, 30, formats["mpix"])
    scale_sheet.set_column(6, 6, 20, formats["ms"])
    scale_sheet.set_column(7, 7, 24, formats["mpix"])

    # Methodology/source sheet for report citation and interpretation.
    methodology = workbook.add_worksheet("Methodology")
    methodology.hide_gridlines(2)
    methodology.set_column("A:A", 28)
    methodology.set_column("B:B", 100)
    methodology.write("A1", "Metric / source", formats["header"])
    methodology.write("B1", "Definition / report note", formats["header"])
    notes = [
        ("Core pipeline median", "Primary tuning metric. Native backend pipeline after input loading and before output writing, including MPI distribution, encoding and merge."),
        ("Encode median", "Primary performance metric. Native QOI Pass 2/encode only; excludes Electron, input decode, output write and validation."),
        ("End-to-end total", "Native conversion latency including load, encode, output write, validation and other native overhead. It is not used for speedup."),
        ("Speedup", "Serial encode median divided by the backend encode median for the same image/configuration."),
        ("Efficiency", "Speedup divided by OpenMP thread count or MPI process count. CUDA is intentionally blank."),
        ("Suite throughput", "Encode throughput uses total pixels divided by the sum of per-image median encode times; the core-pipeline column applies the same calculation to core pipeline time."),
        ("Compression ratio", "Raw pixel bytes divided by encoded QOI bytes. Larger is more compact."),
        ("Correctness", "Official qoi.h decode, dimensions/channels, pixel buffer and SHA-256 checks. Show separately from performance."),
        ("Source", "Consolidated summary/*.csv and tuning-summary/*.csv generated by benchmark/scripts/aggregate_results.py."),
        ("Reproducibility", "One warm-up plus five measured runs per image/configuration; backend processes run sequentially."),
    ]
    for row, (label, note) in enumerate(notes, 1):
        methodology.write(row, 0, label, formats["text"])
        methodology.write(row, 1, note, formats["note"])
    methodology.freeze_panes(1, 0)

    workbook.close()
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
