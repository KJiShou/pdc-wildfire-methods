import { describe, expect, it } from 'vitest'
import { phasePercentages, phaseTotalMilliseconds } from './phaseMetrics'

describe('phasePercentages', () => {
  it('normalizes method phases to 100 percent and excludes loading/validation', () => {
    const percentages = phasePercentages({
      load_ms: 1000,
      cuda_init_ms: 1,
      openmp_init_ms: 2,
      allocation_ms: 1,
      summary_ms: 2,
      propagation_ms: 1,
      transfer_in_ms: 1,
      encode_ms: 2,
      prefix_scan_ms: 1,
      compaction_ms: 1,
      transfer_out_ms: 1,
      merge_ms: 1,
      validation_ms: 1000,
    })

    expect(Object.values(percentages).reduce((sum, value) => sum + value, 0)).toBeCloseTo(100)
    expect(percentages.openmp_init_ms).toBeCloseTo(2 / 14 * 100)
    expect(percentages.encode_ms).toBeCloseTo(2 / 14 * 100)
    expect(percentages.load_ms).toBeUndefined()
    expect(percentages.validation_ms).toBeUndefined()
    expect(phaseTotalMilliseconds({ encode_ms: 2, transfer_in_ms: 1 })).toBe(3)
  })

  it('returns zero shares when no method phase was recorded', () => {
    const percentages = phasePercentages({})

    expect(Object.values(percentages).every((value) => value === 0)).toBe(true)
  })
})
