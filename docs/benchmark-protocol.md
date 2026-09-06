# Benchmark protocol

The automated protocol follows the three-stage evaluation plan in Chapter 5.

## Dataset stages

1. **Correctness:** run Serial, OpenMP, CUDA and MPI over the official QOI
   conformance archive plus RGB/RGBA, transparency, small-image and block-boundary cases.
2. **Tuning:** use the published deterministic 154-image stratified manifest to
   sweep OpenMP threads/image partitions,
   CUDA pixels per segment and CUDA threads per block, and MPI
   processes/image partitions.
3. **Full:** run Serial, one-pass control and the selected best OpenMP, CUDA and
   MPI configurations over every image in the published full manifest (2,848
   images in the current official suite).

The manifests under `benchmark/manifests/` are deterministic input records. Each
entry identifies an image, category and channel mode, while the referenced image
files must already be available locally. Dataset download and synthetic-image
generation are outside the final benchmark execution path.

## Repetition and timing

Every image/configuration performs one unreported warm-up followed by five
measured runs. Aggregation uses per-image median core-pipeline time as the
primary tuning statistic and retains mean and sample standard deviation.
Backends run sequentially. `encode_ms` excludes input decode, output writing
and validation; `write_ms` is reported separately. `core_pipeline_ms` covers
the native backend pipeline after input loading and before output writing,
including MPI distribution, synchronization, encoding and merge. `total_ms` is native end-to-end conversion
latency and excludes Electron, result JSON writing and the separately reported
`metrics_analysis_ms` pass used to collect chunk/state research counters.

For Dashboard-like repeated MPI requests, pass `--persistent-mpi` to
`run_benchmarks.py`. It starts one `mpiexec --server` worker per image and
configuration, sends the warm-up and five measured JSON-line requests through
that worker, and records `request_roundtrip_ms`, worker startup and reuse in
the `experiment` object. The default remains one-shot MPI for compatibility.

`load_ms` measures native input decode. `cuda_init_ms` and `allocation_ms` are
CUDA-only setup phases and are zero for the other backends. `openmp_init_ms` is
the OpenMP-only cost of first thread-team creation/wakeup and is zero for the
other backends. Summary corresponds to Pass 1, propagation is state propagation,
and encode is Pass 2. OpenMP uses the configured worker count with static scheduling for both Pass 1 and Pass 2;
its ordered state propagation remains sequential. For CUDA, `summary_ms`
measures the GPU summary kernel, while `propagation_ms` measures the device
exclusive summary scan and entry-state kernel. `prefix_scan_ms` measures the
encoded-length scan, `compaction_ms` measures device output compaction, and
`merge_ms` measures final host QOI assembly. `core_pipeline_ms` covers transfers,
summary, propagation, encoding, scan and compaction. CUDA reports both pixels per
segment, CUDA threads per block and the derived image partition count. For MPI,
`summary_ms` includes the maximum rank-local summary time plus the summary
gather to rank 0; `transfer_in_ms` includes pixel and propagated-state scatter.
CUDA host/device transfers and the final MPI encoded-payload gather are reported
as transfer in/out. `prefix_scan_ms` remains zero unless a backend actually
performs a separately timed prefix scan; no value is inferred or fabricated.

The Dashboard and Excel phase breakdown normalize the listed method phases
(CUDA init, OpenMP init, allocation, summary, propagation, transfers, encoding,
scan, compaction and merge) to 100% per backend. OpenMP init is included in the
method total but excluded from summary itself. Input loading, output writing, validation and
metrics analysis are excluded from that denominator; native JSON and CSV
contracts retain the original millisecond fields.

## Derived metrics

`aggregate_results.py` produces per-run, per-image, category and full-suite CSV
files. It calculates speedup, CPU/MPI efficiency, equivalent-BMP/QOI compression
ratio, QOI space saved, output-size overhead, chunk distribution, cross-block
counters, encode and core-pipeline suite throughput, and total encoded size.
CUDA efficiency is intentionally blank
because CPU thread/process efficiency is not a meaningful GPU occupancy metric.
Pipeline median/stdev aliases, pipeline speedup/efficiency, and suite pipeline
throughput are retained alongside the native `core_pipeline_*` columns.

## Reproducibility

Each measured JSON is enriched with the exact argument array, source SHA-256,
configuration, stage, category, run index, host platform, processor, logical CPU
count, Python version, native binary directory and single-node declaration.
Compiler flags, GPU model/driver, MPI process placement and background workload
must additionally be recorded in the experiment report because they cannot be
reliably inferred by a portable runner.

Example:

```powershell
python benchmark/scripts/run_benchmarks.py `
  --manifest benchmark/manifests/tuning.json `
  --config benchmark/configs/evaluation.json `
  --stage tuning --native-dir build-full/Release `
  --output-dir results/evaluation --resume

python benchmark/scripts/aggregate_results.py `
  --input-dir results/evaluation --output results/per-run.csv
```

The official sources are `https://qoiformat.org/qoi_test_images.zip` and
`https://qoiformat.org/benchmark/qoi_benchmark_suite.tar`. The benchmark runner
does not download or commit either dataset; it expects the image files referenced
by the selected manifest to have been provisioned separately.
