export type BackendId = 'serial' | 'openmp' | 'cuda' | 'mpi'

export type BackendAvailability = {
  id: BackendId
  label: string
  available: boolean
  reason?: string
}

export type SelectedImage = {
  id: string
  name: string
  size: number
  inputPath: string
  previewDataUrl: string
  width: number
  height: number
  channels: number
}

export type ConversionRequest = {
  imageId: string
  jobId?: string
  backend: BackendId
  blocks?: number
  threads?: number
  segmentLength?: number
  cudaThreadsPerBlock?: number
}

export type NativeResult = {
  status: 'success' | 'validation_failed' | 'error'
  backend: BackendId
  error?: string
  input: { path: string; width: number; height: number; channels: number }
  configuration: {
    blocks: number
    threads: number
    segment_length: number
    cuda_threads_per_block: number
    cuda_device_architecture: string
    persistent_context_reused: boolean
    input_cache_reused: boolean
  }
  timing: {
    load_ms: number
    cuda_init_ms: number
    openmp_init_ms: number
    allocation_ms: number
    summary_ms: number
    propagation_ms: number
    transfer_in_ms: number
    encode_ms: number
    transfer_out_ms: number
    merge_ms: number
    prefix_scan_ms: number
    compaction_ms: number
    core_pipeline_ms: number
    write_ms: number
    metrics_analysis_ms: number
    validation_ms: number
    total_ms: number
  }
  output: { path: string; bytes: number; bmp_bytes: number; compression_ratio: number; throughput_mpixels: number; core_pipeline_throughput_mpixels: number }
  chunks: { run: number; index: number; diff: number; luma: number; rgb: number; rgba: number }
  cross_block: { inherited_index_hits: number; fallback_bytes_avoided: number }
  preview_path: string
  validation: {
    passed: boolean
    decoder_accepted: boolean
    dimensions_match: boolean
    channels_match: boolean
    pixel_match: boolean
  }
}

export type OrchestrationMetrics = {
  request_wall_ms: number
  worker_startup_ms: number
  worker_reused: boolean
  input_cache_reused: boolean
  fallback_used: boolean
}

export type ConversionResponse = {
  jobId: string
  result: NativeResult
  orchestration: OrchestrationMetrics
  previewDataUrl?: string
}
