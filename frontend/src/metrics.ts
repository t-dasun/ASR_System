export function formatBytes(value: number | null | undefined): string {
  if (value === null || value === undefined || !Number.isFinite(value)) return '—'
  if (value < 1024) return `${Math.round(value)} B`
  const units = ['KiB', 'MiB', 'GiB', 'TiB']
  let amount = value
  let unit = -1
  do { amount /= 1024; unit++ } while (amount >= 1024 && unit < units.length - 1)
  return `${amount.toFixed(amount >= 10 ? 1 : 2)} ${units[unit]}`
}

export function formatMs(value: number | null | undefined): string {
  return value === null || value === undefined || !Number.isFinite(value)
    ? '—' : `${value.toFixed(value >= 100 ? 0 : 1)} ms`
}

export function asRecord(value: unknown): Record<string, unknown> {
  return value && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, unknown> : {}
}

export function numberAt(value: unknown, ...keys: string[]): number | null {
  let current: unknown = value
  for (const key of keys) current = asRecord(current)[key]
  return typeof current === 'number' && Number.isFinite(current) ? current : null
}

export function textAt(value: unknown, ...keys: string[]): string {
  let current: unknown = value
  for (const key of keys) current = asRecord(current)[key]
  return typeof current === 'string' ? current : ''
}

export interface CallRecord {
  label: string
  artifactId: string
  language: string
  transcript: string
  status: string
  worker: string
  finalMs: number | null
  firstMs?: number | null
  eofMs?: number | null
  reference?: string
}

export function callsFromSummary(summary: unknown): CallRecord[] {
  const root = asRecord(summary)
  if (typeof root.transcript === 'string') {
    return [{ label: textAt(root, 'run_id') || 'call', artifactId: '', language: '',
      transcript: root.transcript, status: textAt(root, 'status'),
      worker: textAt(root, 'worker_id'),
      firstMs: numberAt(root, 'measurements', 'first_usable_transcript_ns') === null ? null : numberAt(root, 'measurements', 'first_usable_transcript_ns')! / 1e6,
      eofMs: numberAt(root, 'measurements', 'finalization_ns') === null ? null : numberAt(root, 'measurements', 'finalization_ns')! / 1e6,
      reference: textAt(root, 'audio', 'reference'),
      finalMs: numberAt(root, 'measurements', 'final_result_ns') === null ? null :
        numberAt(root, 'measurements', 'final_result_ns')! / 1e6 }]
  }
  const phases = Array.isArray(root.phases) ? root.phases : []
  const calls: CallRecord[] = []
  for (const phase of phases) {
    const entries = asRecord(phase).calls
    if (!Array.isArray(entries)) continue
    for (const entry of entries) {
      const row = asRecord(entry)
      const details = asRecord(row.summary)
      const ns = numberAt(details, 'measurements', 'final_result_ns')
      calls.push({ label: textAt(row, 'run_id') || textAt(row, 'call', 'id'),
        artifactId: textAt(row, 'directory').split('/').filter(Boolean).at(-1) || '',
        language: textAt(row, 'call', 'language'),
        transcript: textAt(details, 'transcript'), status: textAt(row, 'status'),
        worker: textAt(details, 'worker_id'),
        firstMs: numberAt(details, 'measurements', 'first_usable_transcript_ns') === null ? null : numberAt(details, 'measurements', 'first_usable_transcript_ns')! / 1e6,
        eofMs: numberAt(details, 'measurements', 'finalization_ns') === null ? null : numberAt(details, 'measurements', 'finalization_ns')! / 1e6,
        reference: textAt(details, 'audio', 'reference'), finalMs: ns === null ? null : ns / 1e6 })
    }
  }
  return calls
}

export function sparklinePoints(values: number[], width = 240, height = 58): string {
  if (!values.length) return ''
  const max = Math.max(1, ...values)
  return values.map((value, index) => {
    const x = values.length === 1 ? width / 2 : index * width / (values.length - 1)
    const y = height - value / max * (height - 8) - 4
    return `${x.toFixed(1)},${y.toFixed(1)}`
  }).join(' ')
}
