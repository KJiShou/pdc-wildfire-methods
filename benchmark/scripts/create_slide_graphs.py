"""Create slide-ready PNG charts from an aggregated Parallel QOI report.

The script intentionally uses Pillow instead of a plotting framework so it can
run in the same lightweight Python environment as the benchmark scripts.
"""

from __future__ import annotations

import argparse
import csv
import re
from collections import defaultdict
from pathlib import Path
from typing import Iterable

from PIL import Image, ImageDraw, ImageFont


WIDTH = 1600
HEIGHT = 900

COLORS = {
    "serial": "#AEB7C1",
    "openmp": "#789A22",
    "cuda": "#607F16",
    "mpi": "#94AD4D",
    "grid": "#DCE2E7",
    "ink": "#24313C",
    "muted": "#6D7882",
    "panel": "#F7F9FA",
    "accent": "#C56B38",
}
BACKENDS = ["serial", "openmp", "cuda", "mpi"]
LABELS = {"serial": "Serial", "openmp": "OpenMP", "cuda": "CUDA", "mpi": "MPI"}


def load_font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    candidates = [
        Path("C:/Windows/Fonts/arialbd.ttf" if bold else "C:/Windows/Fonts/arial.ttf"),
        Path("C:/Windows/Fonts/segoeuib.ttf" if bold else "C:/Windows/Fonts/segoeui.ttf"),
    ]
    for candidate in candidates:
        if candidate.exists():
            return ImageFont.truetype(str(candidate), size)
    return ImageFont.load_default()


FONT_TITLE = load_font(38, True)
FONT_SUBTITLE = load_font(21)
FONT_PANEL = load_font(24, True)
FONT_BODY = load_font(22)
FONT_SMALL = load_font(17)
FONT_VALUE = load_font(22, True)


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def number(row: dict[str, str], key: str, default: float = 0.0) -> float:
    try:
        return float(row.get(key, ""))
    except (TypeError, ValueError):
        return default


def write_text(draw: ImageDraw.ImageDraw, xy: tuple[int, int], text: str,
               font: ImageFont.ImageFont, fill: str = COLORS["ink"], anchor: str | None = None) -> None:
    draw.text(xy, text, font=font, fill=fill, anchor=anchor)


def title_block(draw: ImageDraw.ImageDraw, title: str, subtitle: str) -> None:
    write_text(draw, (70, 42), title, FONT_TITLE)
    write_text(draw, (70, 96), subtitle, FONT_SUBTITLE, COLORS["muted"])


def footer(draw: ImageDraw.ImageDraw, text: str) -> None:
    write_text(draw, (70, HEIGHT - 38), text, FONT_SMALL, COLORS["muted"])


def full_rows(results_dir: Path) -> dict[str, dict[str, str]]:
    rows = read_csv(results_dir / "summary" / "full-suite-summary.csv")
    return {
        row["backend"]: row
        for row in rows
        if row.get("stage") == "full" and row.get("backend") in BACKENDS
    }


def draw_vertical_bars(draw: ImageDraw.ImageDraw, box: tuple[int, int, int, int],
                       title: str, values: dict[str, float], unit: str,
                       baseline: float | None = None) -> None:
    left, top, right, bottom = box
    draw.rounded_rectangle(box, radius=18, fill=COLORS["panel"], outline=COLORS["grid"], width=2)
    write_text(draw, (left + 28, top + 24), title, FONT_PANEL)

    chart_left = left + 70
    chart_top = top + 90
    chart_right = right - 30
    chart_bottom = bottom - 92
    max_value = max(values.values()) if values else 1.0
    max_value = max_value * 1.18 if max_value > 0 else 1.0

    for tick in range(5):
        ratio = tick / 4
        y = int(chart_bottom - ratio * (chart_bottom - chart_top))
        draw.line((chart_left, y, chart_right, y), fill=COLORS["grid"], width=2)
        label = f"{max_value * ratio:.0f}"
        write_text(draw, (chart_left - 12, y), label, FONT_SMALL, COLORS["muted"], "rm")

    bar_space = (chart_right - chart_left) / max(len(values), 1)
    bar_width = min(110, int(bar_space * 0.56))
    for index, backend in enumerate(BACKENDS):
        if backend not in values:
            continue
        value = values[backend]
        center = chart_left + bar_space * (index + 0.5)
        x0 = int(center - bar_width / 2)
        x1 = int(center + bar_width / 2)
        y1 = chart_bottom
        y0 = int(chart_bottom - (value / max_value) * (chart_bottom - chart_top))
        draw.rounded_rectangle((x0, y0, x1, y1), radius=10, fill=COLORS[backend])
        write_text(draw, (int(center), y0 - 14), f"{value:,.1f}", FONT_VALUE, COLORS["ink"], "ms")
        write_text(draw, (int(center), chart_bottom + 28), LABELS[backend], FONT_BODY, COLORS["ink"], "ma")
        if baseline and backend != "serial":
            write_text(draw, (int(center), chart_bottom + 59), f"{value / baseline:.2f}×", FONT_SMALL, COLORS["muted"], "ma")

    write_text(draw, (chart_left, bottom - 27), unit, FONT_SMALL, COLORS["muted"])


def create_full_suite_performance(results_dir: Path, output_dir: Path) -> None:
    rows = full_rows(results_dir)
    image = Image.new("RGB", (WIDTH, HEIGHT), "white")
    draw = ImageDraw.Draw(image)
    title_block(
        draw,
        "Full-suite performance comparison",
        "2,848 images · 1.322 billion pixels · selected configurations · all outputs pixel-valid",
    )

    encode = {backend: number(rows[backend], "suite_throughput_mpixels") for backend in BACKENDS}
    pipeline = {backend: number(rows[backend], "suite_core_pipeline_throughput_mpixels") for backend in BACKENDS}
    draw_vertical_bars(
        draw,
        (60, 165, 775, 795),
        "Encode-only throughput",
        encode,
        "MPix/s · excludes input load, output write and validation",
        encode["serial"],
    )
    draw_vertical_bars(
        draw,
        (825, 165, 1540, 795),
        "Core-pipeline throughput",
        pipeline,
        "MPix/s · includes transfers, coordination and merge",
        pipeline["serial"],
    )
    footer(
        draw,
        "Interpretation: CUDA is strongest for encode-only work; setup and transfer overhead are visible in the core-pipeline view.",
    )
    image.save(output_dir / "full-suite-performance.png", quality=95)


def grouped_max(rows: Iterable[dict[str, str]], group_key: str, value_key: str) -> dict[int, float]:
    grouped: dict[int, float] = {}
    for row in rows:
        try:
            group = int(float(row.get(group_key, "")))
        except (TypeError, ValueError):
            continue
        value = number(row, value_key)
        grouped[group] = max(grouped.get(group, 0.0), value)
    return grouped


def grouped_config_max(rows: Iterable[dict[str, str]], pattern: str, value_key: str) -> dict[int, float]:
    grouped: dict[int, float] = {}
    compiled = re.compile(pattern)
    for row in rows:
        match = compiled.search(row.get("configuration_id", ""))
        if not match:
            continue
        value = number(row, value_key)
        group = int(match.group(1))
        grouped[group] = max(grouped.get(group, 0.0), value)
    return grouped


def draw_line_panel(draw: ImageDraw.ImageDraw, box: tuple[int, int, int, int], title: str,
                    x_values: list[int], series: dict[str, dict[int, float]],
                    x_label: str, selected: tuple[int, str] | None = None) -> None:
    left, top, right, bottom = box
    draw.rounded_rectangle(box, radius=18, fill=COLORS["panel"], outline=COLORS["grid"], width=2)
    write_text(draw, (left + 24, top + 20), title, FONT_PANEL)
    chart_left = left + 70
    chart_top = top + 84
    chart_right = right - 30
    chart_bottom = bottom - 92
    all_values = [value for values in series.values() for value in values.values()]
    max_value = max(all_values) * 1.18 if all_values else 1.0
    x_step = (chart_right - chart_left) / max(len(x_values) - 1, 1)

    for tick in range(5):
        ratio = tick / 4
        y = int(chart_bottom - ratio * (chart_bottom - chart_top))
        draw.line((chart_left, y, chart_right, y), fill=COLORS["grid"], width=2)
        write_text(draw, (chart_left - 12, y), f"{max_value * ratio:.0f}", FONT_SMALL, COLORS["muted"], "rm")

    for index, value in enumerate(x_values):
        x = int(chart_left + index * x_step)
        draw.line((x, chart_bottom, x, chart_bottom + 8), fill=COLORS["grid"], width=2)
        write_text(draw, (x, chart_bottom + 24), str(value), FONT_SMALL, COLORS["muted"], "ma")

    series_colors = {
        series_name: [COLORS["openmp"], COLORS["cuda"], COLORS["mpi"], COLORS["accent"]][index % 4]
        for index, series_name in enumerate(series)
    }
    for series_name, values in series.items():
        points: list[tuple[int, int]] = []
        for index, x_value in enumerate(x_values):
            if x_value not in values:
                continue
            x = int(chart_left + index * x_step)
            y = int(chart_bottom - values[x_value] / max_value * (chart_bottom - chart_top))
            points.append((x, y))
        color = series_colors[series_name]
        if len(points) >= 2:
            draw.line(points, fill=color, width=5)
        for x, y in points:
            draw.ellipse((x - 7, y - 7, x + 7, y + 7), fill=color, outline="white", width=2)

    legend_x = chart_left
    legend_y = bottom - 29
    legend_step = 120
    for series_name in series:
        color = series_colors[series_name]
        draw.ellipse((legend_x, legend_y - 7, legend_x + 14, legend_y + 7), fill=color)
        write_text(draw, (legend_x + 23, legend_y), series_name, FONT_SMALL, COLORS["muted"], "lm")
        legend_x += legend_step
    write_text(draw, (chart_right, bottom - 8), x_label, FONT_SMALL, COLORS["muted"], "rs")

    if selected and selected[0] in x_values:
        index = x_values.index(selected[0])
        x = int(chart_left + index * x_step)
        draw.line((x, chart_top, x, chart_bottom), fill=COLORS["accent"], width=2)
        write_text(draw, (x, chart_top - 10), selected[1], FONT_SMALL, COLORS["accent"], "ms")


def create_tuning_sweeps(results_dir: Path, output_dir: Path) -> None:
    rows = [row for row in read_csv(results_dir / "tuning-summary" / "full-suite-summary.csv") if row.get("stage") == "tuning"]
    image = Image.new("RGB", (WIDTH, HEIGHT), "white")
    draw = ImageDraw.Draw(image)
    title_block(
        draw,
        "Tuning sweeps",
        "Best observed core-pipeline throughput at each worker/process setting; higher is better",
    )

    openmp_rows = [row for row in rows if row.get("backend") == "openmp"]
    mpi_rows = [row for row in rows if row.get("backend") == "mpi"]
    cuda_rows = [row for row in rows if row.get("backend") == "cuda"]
    openmp = grouped_config_max(openmp_rows, r"openmp_thr-(\d+)", "suite_core_pipeline_throughput_mpixels")
    mpi = grouped_config_max(mpi_rows, r"mpi_pro-(\d+)", "suite_core_pipeline_throughput_mpixels")

    cuda: dict[str, dict[int, float]] = defaultdict(dict)
    for row in cuda_rows:
        match = re.search(r"cuda_seg-(\d+)_cud-(\d+)", row.get("configuration_id", ""))
        if not match:
            continue
        segment = int(match.group(1))
        tpb = int(match.group(2))
        value = number(row, "suite_core_pipeline_throughput_mpixels")
        cuda[str(tpb)][segment] = value

    draw_line_panel(
        draw,
        (60, 170, 520, 790),
        "OpenMP",
        [1, 2, 4, 8, 16],
        {"openmp": openmp},
        "threads",
        (8, "selected"),
    )
    draw_line_panel(
        draw,
        (560, 170, 1040, 790),
        "CUDA",
        [128, 256, 512, 1024, 2048],
        {f"TPB {key}": values for key, values in sorted(cuda.items())},
        "segment length",
        (256, "selected"),
    )
    draw_line_panel(
        draw,
        (1080, 170, 1560, 790),
        "MPI",
        [1, 2, 4, 8],
        {"mpi": mpi},
        "processes",
        (8, "selected"),
    )
    footer(draw, "Selected configurations: OpenMP 8 threads / 32 blocks · CUDA segment 256 / 128 TPB · MPI 8 processes / 8 blocks.")
    image.save(output_dir / "tuning-sweeps.png", quality=95)


def create_median_components(results_dir: Path, output_dir: Path) -> None:
    rows = full_rows(results_dir)
    image = Image.new("RGB", (WIDTH, HEIGHT), "white")
    draw = ImageDraw.Draw(image)
    title_block(
        draw,
        "Per-image median timing",
        "The encode phase measures QOI payload generation; the core pipeline also includes backend coordination and transfers",
    )
    box = (130, 180, 1470, 770)
    draw.rounded_rectangle(box, radius=18, fill=COLORS["panel"], outline=COLORS["grid"], width=2)
    left, top, right, bottom = box
    chart_left = left + 120
    chart_top = top + 100
    chart_right = right - 55
    chart_bottom = bottom - 115
    values = {
        "encode": {backend: number(rows[backend], "encode_ms_median_median") for backend in BACKENDS},
        "core pipeline": {backend: number(rows[backend], "core_pipeline_ms_median_median") for backend in BACKENDS},
    }
    max_value = max(value for series in values.values() for value in series.values()) * 1.2
    for tick in range(5):
        ratio = tick / 4
        y = int(chart_bottom - ratio * (chart_bottom - chart_top))
        draw.line((chart_left, y, chart_right, y), fill=COLORS["grid"], width=2)
        write_text(draw, (chart_left - 14, y), f"{max_value * ratio:.1f}", FONT_SMALL, COLORS["muted"], "rm")
    group_space = (chart_right - chart_left) / len(BACKENDS)
    bar_width = 52
    for index, backend in enumerate(BACKENDS):
        center = chart_left + group_space * (index + 0.5)
        for offset, (series_name, series) in enumerate(values.items()):
            value = series[backend]
            x0 = int(center + (offset - 0.5) * (bar_width + 10) - bar_width / 2)
            x1 = x0 + bar_width
            y1 = chart_bottom
            y0 = int(chart_bottom - value / max_value * (chart_bottom - chart_top))
            color = COLORS["accent"] if series_name == "encode" else COLORS["ink"]
            draw.rounded_rectangle((x0, y0, x1, y1), radius=8, fill=color)
            write_text(draw, (x0 + bar_width // 2, y0 - 12), f"{value:.2f}", FONT_SMALL, COLORS["ink"], "ms")
        write_text(draw, (int(center), chart_bottom + 32), LABELS[backend], FONT_BODY, COLORS["ink"], "ma")

    legend_x = chart_left
    for series_name, color in [("encode", COLORS["accent"]), ("core pipeline", COLORS["ink"])]:
        draw.rectangle((legend_x, bottom - 57, legend_x + 18, bottom - 39), fill=color)
        write_text(draw, (legend_x + 28, bottom - 48), series_name, FONT_SMALL, COLORS["muted"], "lm")
        legend_x += 180
    write_text(draw, (chart_left, bottom - 22), "milliseconds per image", FONT_SMALL, COLORS["muted"])
    footer(draw, "A lower core-pipeline value is better; CUDA's setup and transfer costs are visible outside the encode-only phase.")
    image.save(output_dir / "full-suite-median-timing.png", quality=95)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results-dir", type=Path, required=True, help="final report directory")
    parser.add_argument("--output-dir", type=Path, help="directory for generated PNGs")
    args = parser.parse_args()
    results_dir = args.results_dir.resolve()
    output_dir = (args.output_dir or results_dir / "graphs").resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    create_full_suite_performance(results_dir, output_dir)
    create_tuning_sweeps(results_dir, output_dir)
    create_median_components(results_dir, output_dir)
    print(f"created slide graphs in {output_dir}")


if __name__ == "__main__":
    main()
