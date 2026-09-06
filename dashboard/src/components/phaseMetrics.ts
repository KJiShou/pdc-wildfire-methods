export const PERFORMANCE_PHASE_KEYS = [
  'cuda_init_ms',
  'openmp_init_ms',
  'allocation_ms',
  'summary_ms',
  'propagation_ms',
  'transfer_in_ms',
  'encode_ms',
  'prefix_scan_ms',
  'compaction_ms',
  'transfer_out_ms',
  'merge_ms',
] as const

function finiteNonNegative(value: unknown): number {
  const numeric = Number(value)
  return Number.isFinite(numeric) && numeric > 0 ? numeric : 0
}

export function phaseTotalMilliseconds(timing: Record<string, unknown>): number {
  return PERFORMANCE_PHASE_KEYS.reduce((sum, key) => sum + finiteNonNegative(timing[key]), 0)
}

export function phasePercentages(timing: Record<string, unknown>): Record<string, number> {
  const values = Object.fromEntries(PERFORMANCE_PHASE_KEYS.map((key) => [key, finiteNonNegative(timing[key])]))
  const total = phaseTotalMilliseconds(timing)
  if (total <= 0) return values
  return Object.fromEntries(Object.entries(values).map(([key, value]) => [key, (value / total) * 100]))
}
