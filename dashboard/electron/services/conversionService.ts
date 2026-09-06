import { existsSync } from 'node:fs'
import type { BackendRegistry } from './backendRegistry'
import { ProcessRunner } from './processRunner'
import { TempFileService } from './tempFileService'
import type { BackendId, ConversionRequest, ConversionResponse, NativeResult, OrchestrationMetrics, SelectedImage } from './types'

function finiteNonNegative(value: unknown): number {
  const numeric = Number(value)
  return Number.isFinite(numeric) && numeric >= 0 ? numeric : 0
}

function bool(value: unknown): boolean { return value === true }

type NativeResultInput = Omit<Partial<NativeResult>, 'timing' | 'output' | 'validation' | 'configuration'> & {
  timing?: Partial<NativeResult['timing']>
  output?: Partial<NativeResult['output']>
  validation?: Partial<NativeResult['validation']>
  configuration?: Partial<NativeResult['configuration']>
}

export function normalizeResult(raw: NativeResultInput, request: ConversionRequest, image: SelectedImage, job: { outputPath: string; previewPath: string }): NativeResult {
  const rawTiming = raw.timing
  const rawValidation = raw.validation
  const rawConfiguration = raw.configuration
  const rawOutput = raw.output
  const input = raw.input ?? { path: image.inputPath, width: image.width, height: image.height, channels: image.channels }
  const inputWidth = Number(input.width ?? image.width)
  const inputHeight = Number(input.height ?? image.height)
  const outputBytes = finiteNonNegative(rawOutput?.bytes)
  const fallbackBmpBytes = Number.isFinite(inputWidth) && inputWidth >= 0 && Number.isFinite(inputHeight) && inputHeight >= 0
    ? 54 + inputWidth * inputHeight * 4
    : 0
  const bmpBytes = finiteNonNegative(rawOutput?.bmp_bytes ?? fallbackBmpBytes)
  const threads = request.backend === 'serial' ? 1 : (request.threads ?? 4)
  const segmentLength = Math.max(1, request.segmentLength ?? 1024)
  const requestedBlocks = Math.max(1, request.blocks ?? 8)
  const cudaThreadsPerBlock = Math.max(32, request.cudaThreadsPerBlock ?? 128)
  const blocks = request.backend === 'serial'
    ? 1
    : request.backend === 'cuda'
      ? Math.ceil((image.width * image.height) / segmentLength)
      : request.backend === 'mpi' ? Math.max(requestedBlocks, threads) : requestedBlocks
  return {
    status: raw.status === 'success' || raw.status === 'validation_failed' ? raw.status : 'error',
    backend: raw.backend ?? request.backend,
    error: raw.error,
    input,
    configuration: {
      blocks: Number(rawConfiguration?.blocks ?? blocks),
      threads: Number(rawConfiguration?.threads ?? threads),
      segment_length: Number(rawConfiguration?.segment_length ?? segmentLength),
      cuda_threads_per_block: Number(rawConfiguration?.cuda_threads_per_block ?? cudaThreadsPerBlock),
      cuda_device_architecture: rawConfiguration?.cuda_device_architecture ?? '',
      persistent_context_reused: bool(rawConfiguration?.persistent_context_reused),
      input_cache_reused: bool(rawConfiguration?.input_cache_reused),
    },
    timing: {
      load_ms: finiteNonNegative(rawTiming?.load_ms),
      cuda_init_ms: finiteNonNegative(rawTiming?.cuda_init_ms),
      openmp_init_ms: finiteNonNegative(rawTiming?.openmp_init_ms),
      allocation_ms: finiteNonNegative(rawTiming?.allocation_ms),
      summary_ms: finiteNonNegative(rawTiming?.summary_ms),
      propagation_ms: finiteNonNegative(rawTiming?.propagation_ms),
      transfer_in_ms: finiteNonNegative(rawTiming?.transfer_in_ms),
      encode_ms: finiteNonNegative(rawTiming?.encode_ms),
      transfer_out_ms: finiteNonNegative(rawTiming?.transfer_out_ms),
      merge_ms: finiteNonNegative(rawTiming?.merge_ms),
      prefix_scan_ms: finiteNonNegative(rawTiming?.prefix_scan_ms),
      compaction_ms: finiteNonNegative(rawTiming?.compaction_ms),
      core_pipeline_ms: finiteNonNegative(rawTiming?.core_pipeline_ms),
      write_ms: finiteNonNegative(rawTiming?.write_ms),
      metrics_analysis_ms: finiteNonNegative(rawTiming?.metrics_analysis_ms),
      validation_ms: finiteNonNegative(rawTiming?.validation_ms),
      total_ms: finiteNonNegative(rawTiming?.total_ms),
    },
    output: {
      path: rawOutput?.path ?? job.outputPath,
      bytes: outputBytes,
      bmp_bytes: bmpBytes,
      compression_ratio: outputBytes > 0 ? bmpBytes / outputBytes : 0,
      throughput_mpixels: Number(rawOutput?.throughput_mpixels ?? 0),
      core_pipeline_throughput_mpixels: Number(rawOutput?.core_pipeline_throughput_mpixels ?? 0),
    },
    chunks: raw.chunks ?? { run: 0, index: 0, diff: 0, luma: 0, rgb: 0, rgba: 0 },
    cross_block: raw.cross_block ?? { inherited_index_hits: 0, fallback_bytes_avoided: 0 },
    preview_path: raw.preview_path ?? job.previewPath,
    validation: {
      passed: rawValidation?.passed ?? false,
      decoder_accepted: rawValidation?.decoder_accepted ?? false,
      dimensions_match: rawValidation?.dimensions_match ?? false,
      channels_match: rawValidation?.channels_match ?? false,
      pixel_match: rawValidation?.pixel_match ?? false,
    },
  }
}

function normalizeOrchestration(raw: Record<string, unknown>, result: NativeResult): OrchestrationMetrics {
  const rawMetrics = raw.__orchestration as Partial<OrchestrationMetrics> | undefined
  return {
    request_wall_ms: finiteNonNegative(rawMetrics?.request_wall_ms),
    worker_startup_ms: finiteNonNegative(rawMetrics?.worker_startup_ms),
    worker_reused: bool(rawMetrics?.worker_reused),
    input_cache_reused: bool(rawMetrics?.input_cache_reused) || result.configuration.input_cache_reused,
    fallback_used: bool(rawMetrics?.fallback_used),
  }
}

export class ConversionService {
  private readonly selectedImages = new Map<string, SelectedImage>()
  private readonly results = new Map<string, NativeResult>()

  constructor(private readonly registry: BackendRegistry, private readonly runner: ProcessRunner, private readonly temp: TempFileService) {}

  registerImage(image: SelectedImage): void { this.selectedImages.set(image.id, image) }

  removeImage(imageId: string): void { this.selectedImages.delete(imageId) }

  image(imageId: string): SelectedImage | undefined { return this.selectedImages.get(imageId) }

  async convert(request: ConversionRequest): Promise<ConversionResponse> {
    const image = this.selectedImages.get(request.imageId)
    if (!image) throw new Error('selected image is no longer available')
    const executable = this.registry.executablePath(request.backend)
    if (!executable) throw new Error(`${request.backend} backend is not available on this machine`)
    const job = await this.temp.createJob(request.jobId)
    const threads = request.backend === 'serial' ? 1 : (request.threads ?? 4)
    const requestedBlocks = request.blocks ?? 8
    const blocks = request.backend === 'serial'
      ? 1
      : request.backend === 'mpi'
        ? Math.max(requestedBlocks, threads)
        : request.backend === 'cuda' ? 1 : requestedBlocks
    const args = [
      '--input', image.inputPath,
      '--output', job.outputPath,
      '--result', job.resultPath,
      '--preview', job.previewPath,
      '--threads', String(threads),
      '--segment-length', String(request.segmentLength ?? 1024),
      '--validate',
    ]
    if (request.backend === 'cuda') args.push('--cuda-threads-per-block', String(request.cudaThreadsPerBlock ?? 128))
    if (request.backend !== 'cuda') args.push('--blocks', String(blocks))
    let command = executable
    let commandArgs = args
    if (request.backend === 'mpi') {
      const launcher = await this.registry.mpiLauncher()
      if (!launcher) throw new Error('mpiexec was not found on this machine')
      command = launcher
      commandArgs = ['-n', String(request.threads ?? 4), executable, ...args]
    }
    const rawResult = request.backend === 'cuda'
      ? await this.runner.runCuda(job.jobId, executable, {
          request_id: job.jobId,
          input: image.inputPath,
          output: job.outputPath,
          result: job.resultPath,
          preview: job.previewPath,
          segment_length: request.segmentLength ?? 1024,
          cuda_threads_per_block: request.cudaThreadsPerBlock ?? 128,
          validate: true,
        }, job.resultPath)
      : request.backend === 'mpi'
        ? await this.runner.runMpi(job.jobId, executable, command, request.threads ?? 4, {
            request_id: job.jobId,
            input: image.inputPath,
            output: job.outputPath,
            result: job.resultPath,
            preview: job.previewPath,
            blocks,
            segment_length: request.segmentLength ?? 1024,
            validate: true,
          }, job.resultPath, commandArgs)
      : await this.runner.run(job.jobId, command, commandArgs, job.resultPath)
    const result = normalizeResult(rawResult as NativeResultInput, request, image, job)
    const orchestration = normalizeOrchestration(rawResult, result)
    const previewDataUrl = result.validation?.passed && existsSync(job.previewPath)
      ? await this.temp.previewDataUrl(job.jobId)
      : undefined
    this.results.set(job.jobId, result)
    return { jobId: job.jobId, result, orchestration, previewDataUrl }
  }

  async compare(requests: ConversionRequest[]): Promise<ConversionResponse[]> {
    if (requests.length === 0) throw new Error('select at least one backend to compare')
    const serialIncluded = requests.some((request) => request.backend === 'serial')
    const ordered = serialIncluded ? requests : [{ ...requests[0], backend: 'serial' as BackendId }, ...requests]
    const responses: ConversionResponse[] = []
    for (const request of ordered) responses.push(await this.convert(request))
    return responses
  }

  cancel(jobId: string): boolean { return this.runner.cancel(jobId) }

  async save(jobId: string): Promise<string | undefined> {
    const result = this.results.get(jobId)
    if (!result?.validation.passed) throw new Error('QOI output did not pass validation and cannot be saved')
    return this.temp.save(jobId)
  }
}
