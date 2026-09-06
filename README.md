# Parallel QOI Converter

An interactive Windows Electron application for converting PNG/BMP/JPEG images to
the standard Quite OK Image Format (QOI), validating the decoded pixels, and
comparing Serial, OpenMP, CUDA, and MPI execution models.

The repository has two runtime layers:

- `src/` and `include/` contain the C++17 native core, shared QOI primitives,
  validation, metrics, and the four backend algorithms.
- `dashboard/` contains the Electron main process and the Arco Design React
  renderer. The renderer uses a restricted preload API and never starts native
  processes or reads arbitrary files directly.

## Backend algorithm files

These are the main files to edit when working on parallel algorithms:

| Backend | Algorithm file | Responsibility |
| --- | --- | --- |
| Serial | `src/backends/serial_encoder.cpp` | Sequential block encoding and correctness baseline |
| OpenMP | `src/backends/openmp_encoder.cpp` | Static-scheduled CPU block summary and encoding |
| CUDA | `src/backends/cuda_encoder.cu` | GPU segment kernels, transfers, prefix scan, and device output merge |
| MPI | `src/backends/mpi_encoder.cpp` | Rank distribution, local encoding, gather, and rank-0 merge |

The shared code in `src/core/` owns image loading, block partitioning, QOI
state propagation, QOI chunk primitives, output assembly, validation, and JSON
metrics. `src/core/qoi_codec.cpp` is deliberately shared; it is not a fifth
backend algorithm.

One-pass remains an internal native experiment through `pqoi_control` and is
not shown in the Electron product UI.

## Requirements

For the Windows development build:

- Visual Studio 2022 C++ workload
- CMake 3.21 or newer
- Node.js and pnpm for the dashboard
- NVIDIA CUDA Toolkit and a compatible GPU for CUDA builds
- Microsoft MPI runtime, SDK, headers, and libraries for MPI builds

The current full preset contains native CUDA code for Compute Capabilities 8.6
(RTX 3050 Ti) and 8.9 (RTX 4060). CUDA Toolkit 11.8 or newer is required for
the 8.9 target.

## Build the native backends

Serial and OpenMP build:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-release
ctest --test-dir build-msvc -C Release --output-on-failure
```

Complete CUDA and MPI build:

```powershell
cmake --preset windows-full
cmake --build --preset windows-full-release
ctest --test-dir build-full -C Release --output-on-failure
```

The executables are written to `build-msvc/Release` or `build-full/Release`.

## Run the native executables

Run the commands below from the repository root, `parallel-qoi/`. Use quotes
around any path that contains spaces. PowerShell's call operator (`&`) is used
so that quoted executable paths work reliably.

### Which executable should I run?

| Executable | Build directory | Purpose | Main controls |
| --- | --- | --- | --- |
| `pqoi_serial.exe` | `build-msvc/Release` or `build-full/Release` | Sequential correctness baseline | No tuning is normally required; default is one block |
| `pqoi_control.exe` | `build-msvc/Release` or `build-full/Release` | Internal block-local one-pass control | Used for research comparison, not shown in the dashboard |
| `pqoi_openmp.exe` | `build-msvc/Release` or `build-full/Release` | Shared-memory CPU implementation | `--threads`, `--blocks` |
| `pqoi_cuda.exe` | `build-full/Release` | CUDA GPU implementation | `--segment-length`, `--cuda-threads-per-block` |
| `pqoi_mpi.exe` | `build-full/Release` | MPI distributed-process implementation | `mpiexec -n`, `--blocks` |
| `pqoi_decode_preview.exe` | `build-msvc/Release` or `build-full/Release` | Decode a QOI file to BMP and optionally compare it with an input image | Positional arguments only |

The normal conversion command has this shape:

```text
<executable> --input <image> --output <qoi-file> [options]
```

### Common conversion arguments

| Argument | Required | Meaning |
| --- | --- | --- |
| `--help`, `-h` | No | Print the executable's usage text and exit. |
| `--input <path>` | Yes | Input PNG, BMP, or JPEG image. The path may be relative or absolute. |
| `--output <path>` | Yes | Destination QOI file. Existing files at this path are replaced. |
| `--result <path>` | No | Result JSON path. Default: `<output>.json`, for example `out.qoi.json`. |
| `--preview <path>` | No | Destination decoded BMP preview. It is written only when `--validate` is enabled and validation succeeds. |
| `--no-preview` | No | Do not create the default preview path. Use this for benchmark runs that do not need BMP previews. |
| `--validate` | No | Decode the generated QOI and compare the decoder result with the input image. A successful validation sets `validation.passed` to `true` in the result JSON. |
| `--threads <count>` | No | OpenMP worker-thread count. Default: `1`. For MPI, use `mpiexec -n <count>` to select the process count instead. It is not a tuning control for Serial or CUDA. |
| `--blocks <count>` | No | Number of image partitions for OpenMP or MPI. OpenMP defaults to roughly `2 × threads`; MPI defaults to at least one partition per MPI process. The effective count is capped by the number of pixels. Serial normally uses one block; CUDA derives its partitions from `--segment-length`. |
| `--segment-length <pixels>` | No | CUDA partition-size control. Default: `1024`. The CUDA backend derives approximately `ceil(pixel_count / segment_length)` image partitions. It is ignored by the CPU backends. |
| `--cuda-threads-per-block <count>` | No | CUDA kernel launch size. Default: `128`. It must be at least `32`, a multiple of `32`, and no larger than the selected GPU's device limit. It is used only by CUDA. |

The native program writes a result JSON even when conversion fails, when a
result path is available. The JSON contains the backend configuration, timing
phases, QOI/BMP output sizes, compression ratio, throughput, QOI chunk counts,
and pixel validation flags. See
`benchmark/schemas/benchmark-result.schema.json` for the complete schema.

The usual exit codes are:

| Exit code | Meaning |
| --- | --- |
| `0` | Conversion succeeded, or validation was not requested |
| `1` | Conversion failed or requested validation failed |
| `2` | Invalid command-line arguments or another CLI-level error |

### Serial example

This creates a QOI file, a result JSON, and a validated decoded BMP preview:

```powershell
& ".\build-msvc\Release\pqoi_serial.exe" `
  --input ".\image.bmp" `
  --output ".\results\serial.qoi" `
  --result ".\results\serial.json" `
  --preview ".\results\serial.bmp" `
  --validate
```

### OpenMP example

This requests four CPU worker threads and eight image partitions:

```powershell
& ".\build-msvc\Release\pqoi_openmp.exe" `
  --input ".\image.bmp" `
  --output ".\results\openmp.qoi" `
  --result ".\results\openmp.json" `
  --preview ".\results\openmp.bmp" `
  --threads 4 `
  --blocks 8 `
  --validate
```

For performance measurements, keep `--validate` when correctness must be
recorded. The benchmark runner uses validation but omits previews for most
runs so that preview file I/O does not dominate the experiment.

### CUDA example

CUDA requires the full build and a compatible NVIDIA GPU:

```powershell
& ".\build-full\Release\pqoi_cuda.exe" `
  --input ".\image.bmp" `
  --output ".\results\cuda.qoi" `
  --result ".\results\cuda.json" `
  --preview ".\results\cuda.bmp" `
  --segment-length 1024 `
  --cuda-threads-per-block 128 `
  --validate
```

`--segment-length` controls how many pixels are assigned to a logical CUDA
segment. `--cuda-threads-per-block` controls the CUDA kernel launch shape;
these are different controls and should be tuned separately.

### MPI example

MPI must be started through `mpiexec`. The `-n 4` value selects four MPI
processes; do not use `--threads` as a replacement for `-n`:

```powershell
mpiexec -n 4 ".\build-full\Release\pqoi_mpi.exe" `
  --input ".\image.bmp" `
  --output ".\results\mpi.qoi" `
  --result ".\results\mpi.json" `
  --preview ".\results\mpi.bmp" `
  --blocks 8 `
  --validate
```

For MPI, `--blocks` controls the image partition count. If it is smaller than
the process count, the implementation raises the effective partition count so
that every process can receive work.

### One-pass control example

`pqoi_control.exe` uses the same common arguments but intentionally encodes
each block with block-local state. It is useful as a research/control row in a
benchmark, not as the normal product backend:

```powershell
& ".\build-msvc\Release\pqoi_control.exe" `
  --input ".\image.bmp" `
  --output ".\results\control.qoi" `
  --result ".\results\control.json" `
  --no-preview `
  --validate
```

### Decode and inspect a QOI output

`pqoi_decode_preview.exe` takes positional arguments rather than the common
`--input`/`--output` flags:

```powershell
& ".\build-msvc\Release\pqoi_decode_preview.exe" `
  ".\results\openmp.qoi" `
  ".\results\openmp-decoded.bmp" `
  ".\image.bmp"
```

The third argument is optional. When supplied, the program compares the
decoded pixels with that expected image and returns exit code `1` if any pixel
differs.

### CUDA/MPI persistent server mode

`pqoi_cuda.exe --server` and
`mpiexec -n <processes> pqoi_mpi.exe --server` start the line-oriented worker
protocol used by Electron and the benchmark runner for repeated requests.
Use the ordinary commands above for one-off manual conversions; server mode
expects JSON-line requests on standard input.

## Run the Electron dashboard

```powershell
cd dashboard
pnpm install
pnpm run dev
```

The dashboard provides:

- **Convert**: upload one PNG/BMP/JPEG, choose a backend, preview decoded QOI, and
  save only after validation succeeds.
- **Compare**: run selected backends sequentially and compare encode runtime,
  throughput, speedup, QOI/BMP sizes, phase shares, and validation. The phase
  chart normalizes method phases to 100% and excludes image loading, output
  writing, validation and metrics analysis.
- **Performance charts**: Runtime, Throughput, and Phase Breakdown tabs using
  existing native result data without network requests.

The Electron main process detects executable, CUDA, and MPI availability. It
keeps a persistent CUDA worker so CUDA initialization and reusable buffer
allocation are amortized across conversions. An
unavailable backend stays visible but disabled with a reason; Serial is the
fallback correctness baseline.

## Project layout

```text
parallel-qoi/
├── include/pqoi/       C++ public interfaces and data structures
├── src/core/           Shared image, QOI, validation, metrics, and CLI code
├── src/backends/       Serial, OpenMP, CUDA, and MPI algorithms
├── src/cli/            Executable entry points
├── dashboard/electron/ Electron main process, IPC, and native process runner
├── dashboard/src/      Arco Design renderer pages and components
├── benchmark/          Benchmark scripts, config, and result schema
├── tests/               Native unit tests
└── third_party/        Vendored QOI and stb_image dependencies
```

Generated build directories, dashboard dependencies, packaged output, and
benchmark result files are excluded by `.gitignore`.

## Reproducible evaluation

The `benchmark/` pipeline implements the Chapter 5 three-stage evaluation:
official conformance images, a deterministic stratified tuning subset, and the
full benchmark suite using selected configurations. It performs one warm-up
and five measured runs, launches MPI through `mpiexec`, and reports per-image
median/mean/standard deviation plus category and full-suite summaries.

On Windows, run the complete report pipeline from the repository root:

```powershell
.\run-final-benchmark.cmd
```

This launcher configures and builds the `windows-full` preset before running
native tests, smoke validation, formal correctness, tuning, automatic
best-configuration selection, the full suite, aggregation, and Excel report
generation. In other words, it compiles the native executables before the
benchmark unless the underlying PowerShell script is called with `-SkipBuild`.
The full run requires the CUDA Toolkit, a compatible NVIDIA GPU, MPI/
`mpiexec`, Python, CMake, and CTest. Results are stored under
`results/final-<git-commit>`. Run the launcher again after an interruption to
resume successful artifacts.

To run the orchestrator directly, which exposes optional switches such as
`-SkipBuild`, `-SkipSmoke`, `-SkipCorrectness`, `-SkipTuning`, `-SkipFull`, and
`-DryRun`, use:

```powershell
powershell -NoLogo -NoProfile -ExecutionPolicy Bypass `
  -File .\benchmark\scripts\run_final_evaluation.ps1
```

Do not use `-SkipBuild` when the benchmark must represent the current source
tree. The orchestrator records the current Git commit in the result directory.

```powershell
python benchmark/scripts/run_benchmarks.py `
  --manifest benchmark/manifests/tuning.json `
  --config benchmark/configs/evaluation.json `
  --stage tuning `
  --native-dir build-full/Release `
  --output-dir results/evaluation `
  --mpi-launcher mpiexec `
  --resume

python benchmark/scripts/aggregate_results.py `
  --input-dir results/evaluation `
  --output results/per-run.csv `
  --summary-dir results/summary
```

See `docs/benchmark-protocol.md` for dataset manifest generation, parameter
sweeps, timing boundaries, derived metrics and reproducibility requirements.
