# Result format

`result.json` is the stable contract between C++, Electron and the benchmark
pipeline. It contains:

- input dimensions and RGB/RGBA channel mode;
- backend configuration;
- persistent MPI/CUDA context and input-cache reuse flags (missing legacy flags
  are treated as false by the Dashboard);
- input decode (`load_ms`), CUDA initialization/allocation when applicable,
  OpenMP thread-team initialization (`openmp_init_ms`) when applicable,
  Pass 1/summary, propagation, transfer, Pass 2/encode, prefix scan,
  compaction, core CUDA pipeline, merge, file write, validation and end-to-end
  timing;
- core pipeline timing (`core_pipeline_ms`) and its throughput, covering the
  native encode pipeline without load, write, validation or metrics analysis;
- QOI output size, equivalent uncompressed 32-bit BMP size, compression ratio
  (`BMP bytes / QOI bytes`), and encode throughput;
- raw timing fields remain in milliseconds; phase-share percentages are derived
  only by the Dashboard and Excel presentation layers;
- RUN, INDEX, DIFF, LUMA, RGB and RGBA chunk counts;
- inherited cross-block INDEX hits and fallback bytes avoided;
- official-decoder, dimensions/channels, and complete pixel-buffer correctness
  flags. No cryptographic hash is used for output validation.

The benchmark runner adds an `experiment` object to each artifact. It records
stage, image/category identifiers, warm-up status, measured-run index, source
digest, exact command, configuration and host metadata. The complete contract
is in `benchmark/schemas/benchmark-result.schema.json`.

Dashboard responses additionally expose orchestration timing: request wall time,
worker startup, worker reuse, input-cache reuse and one-shot fallback. These
values are separate from native `core_pipeline_ms` so algorithm time is not
confused with MPI process startup or Electron scheduling.
