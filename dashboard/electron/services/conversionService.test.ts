import { describe, expect, it } from 'vitest'
import { normalizeResult } from './conversionService'
import type { ConversionRequest, SelectedImage } from './types'

const image: SelectedImage = {
  id: 'image-1', name: 'input.bmp', size: 12, inputPath: 'input.bmp',
  previewDataUrl: '', width: 8, height: 4, channels: 4,
}
const request: ConversionRequest = { imageId: image.id, backend: 'mpi', blocks: 8, threads: 2 }
const job = { outputPath: 'output.qoi', previewPath: 'preview.bmp' }

describe('normalizeResult', () => {
  it('defaults orchestration-compatible fields for legacy native JSON', () => {
    const result = normalizeResult({ status: 'success', backend: 'mpi' }, request, image, job)
    expect(result.configuration.input_cache_reused).toBe(false)
    expect(result.configuration.persistent_context_reused).toBe(false)
    expect(result.timing.openmp_init_ms).toBe(0)
    expect(result.output.core_pipeline_throughput_mpixels).toBe(0)
  })

  it('preserves persistent context and cache flags from the native result', () => {
    const result = normalizeResult({
      status: 'success',
      configuration: {
        blocks: 11, threads: 2, segment_length: 1024, cuda_threads_per_block: 128,
        cuda_device_architecture: '', persistent_context_reused: true, input_cache_reused: true,
      },
      output: { path: 'output.qoi', bytes: 99, bmp_bytes: 182, compression_ratio: 1.2, throughput_mpixels: 4.5, core_pipeline_throughput_mpixels: 7.8 },
    }, request, image, job)
    expect(result.configuration.blocks).toBe(11)
    expect(result.configuration.persistent_context_reused).toBe(true)
    expect(result.configuration.input_cache_reused).toBe(true)
    expect(result.output.core_pipeline_throughput_mpixels).toBe(7.8)
  })

  it('derives the equivalent BMP size and compression ratio for legacy native JSON', () => {
    const result = normalizeResult({
      status: 'success',
      output: { path: 'output.qoi', bytes: 100, compression_ratio: 999, throughput_mpixels: 0, core_pipeline_throughput_mpixels: 0 },
    }, request, image, job)

    expect(result.output.bmp_bytes).toBe(54 + 8 * 4 * 4)
    expect(result.output.compression_ratio).toBe((54 + 8 * 4 * 4) / 100)
  })
})
