export type Language = 'en' | 'id' | 'zh'

export interface Capabilities {
  schema_version: number
  engine: string
  engine_revision: string
  streaming_kind: string
  is_mock: boolean
  device: string
  precision: string
  languages: Language[]
  sample_rate_hz: number
  worker_processes: number
  manifest_inputs?: number
  worker_routing?: string
  decode_scheduling?: string
  max_sessions_per_process?: number
  process_isolated?: boolean
  hard_decode_watchdog?: boolean
  cooperative_cancellation?: boolean
}

export interface WorkerSnapshot {
  worker_id: string
  call_id: string
  language: string
  state: string
  occupied: boolean
  healthy: boolean
  max_sessions?: number
  active_sessions: number
  runtime_threads: number
  process_id: number
  failures: number
  last_error: string
  server_queue_depth: number | null
  calls?: { call_id: string; language: string; state: string; buffered_samples: number; decoding: boolean }[]
}

export interface RuntimeSnapshot {
  schema_version: number
  timestamp_ns?: number
  workers: WorkerSnapshot[]
  system: {
    host_cpu_percent: number | null
    host_memory_available_bytes: number
    host_memory_total_bytes: number
    sampled_process_tree_rss_bytes: number
    queue_depth_available: boolean
  } | null
}

export interface HistoryItem {
  id: string
  status?: { status?: string }
}

export interface SuiteRequest {
  kind: 'load' | 'sweep'
  mode: 'direct' | 'network'
  calls: number
  concurrency: number
  warmups: number
  repetitions: number
  languages: Language[]
  overrides: string[]
  strategy?: 'baseline' | 'oat' | 'matrix' | 'scale' | 'selected'
  axes?: { key: string; values: string[] }[]
  max_failure_rate?: number
  max_p95_final_ms?: number
  idempotency_key?: string
}

export interface JobStatus {
  job_id: string
  status: string
  result?: Record<string, unknown>
  error?: string
}

export function normalizeBase(input: string): string {
  const url = new URL(input.trim())
  if (url.protocol !== 'http:' || !['127.0.0.1', 'localhost'].includes(url.hostname)) {
    throw new Error('The M7 service is loopback-only. Use http://127.0.0.1:<port>.')
  }
  if (!url.port || url.pathname !== '/' || url.search || url.hash) {
    throw new Error('Enter only the service origin, including its port.')
  }
  return url.origin
}

export function socketUrl(base: string, path: string): string {
  return `${base.replace(/^http:/, 'ws:')}${path}`
}

export async function requestJson<T>(base: string, path: string, body?: unknown): Promise<T> {
  const response = await fetch(`${base}${path}`, {
    method: body === undefined ? 'GET' : 'POST',
    headers: body === undefined ? undefined : { 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
    cache: 'no-store',
  })
  const data: unknown = await response.json()
  if (!response.ok) {
    const message =
      typeof data === 'object' && data !== null && 'error' in data
        ? String((data as { error: unknown }).error)
        : `HTTP ${response.status}`
    throw new Error(message)
  }
  return data as T
}

export async function downloadArtifact(base: string, id: string, name: string,
                                       callId?: string): Promise<void> {
  if (!/^[a-zA-Z0-9_-]+$/.test(id) || !/^[a-zA-Z0-9_.-]+$/.test(name) ||
      (callId !== undefined && !/^[a-zA-Z0-9_-]+$/.test(callId))) {
    throw new Error('Invalid artifact identifier')
  }
  const response = await fetch(`${base}/v1/artifacts/${id}/${callId ? `${callId}/` : ''}${name}`,
    { cache: 'no-store' })
  if (!response.ok) throw new Error(`Artifact unavailable (HTTP ${response.status})`)
  const blob = await response.blob()
  const href = URL.createObjectURL(blob)
  const anchor = document.createElement('a')
  anchor.href = href
  anchor.download = `${id}_${callId ? `${callId}_` : ''}${name}`
  anchor.click()
  URL.revokeObjectURL(href)
}
