import { useEffect, useMemo, useState } from 'react'
import { requestJson, type Capabilities, type JobStatus, type Language, type SuiteRequest } from '../api'
import { summarizeAccuracy } from '../accuracy'
import { callsFromSummary, asRecord, formatBytes, formatMs, numberAt, textAt } from '../metrics'
import type { LogWriter } from '../types'

interface Props { base: string; capabilities: Capabilities | null; log: LogWriter; onComplete: () => void }

export function ExperimentPanel({ base, capabilities, log, onComplete }: Props) {
  const [kind, setKind] = useState<'load' | 'sweep'>('load')
  const [mode, setMode] = useState<'direct' | 'network'>('direct')
  const [calls, setCalls] = useState(2)
  const [concurrency, setConcurrency] = useState(1)
  const [processes, setProcesses] = useState(1)
  const [warmups, setWarmups] = useState(0)
  const [repetitions, setRepetitions] = useState(1)
  const [languages, setLanguages] = useState<Language[]>(['en'])
  const [strategy, setStrategy] = useState<'baseline' | 'oat' | 'matrix' | 'scale'>('baseline')
  const [axisKey, setAxisKey] = useState('audio.chunk_ms')
  const [axisValues, setAxisValues] = useState('100,200,500')
  const [chunkMs, setChunkMs] = useState(200)
  const [decodeMs, setDecodeMs] = useState(4000)
  const [extraOverrides, setExtraOverrides] = useState('')
  const [failureBound, setFailureBound] = useState('')
  const [finalBound, setFinalBound] = useState('')
  const [resolved, setResolved] = useState<unknown>(null)
  const [plan, setPlan] = useState<unknown>(null)
  const [job, setJob] = useState<JobStatus | null>(null)
  const [error, setError] = useState('')
  const [busy, setBusy] = useState(false)
  const previewMode = capabilities?.engine?.startsWith('qwen_prefix_') || false
  const sharedModel = capabilities?.shared_model || previewMode || capabilities?.engine?.startsWith('qwen_stream_') || false
  const manifestInputs = capabilities?.manifest_inputs || 0
  const runnerLimit = capabilities?.max_load_concurrency || 64
  const activeLimit = sharedModel ? Math.min(runnerLimit, (capabilities?.max_sessions_per_process || 1) * (capabilities?.worker_processes || 1)) : runnerLimit

  useEffect(() => {
    setChunkMs(capabilities?.chunk_ms || 200)
    setDecodeMs(previewMode ? capabilities?.prefix_preview_ms || 4000 : capabilities?.decode_step_ms || 2000)
  }, [capabilities?.chunk_ms, capabilities?.prefix_preview_ms, capabilities?.decode_step_ms, previewMode])

  useEffect(() => { setProcesses(capabilities?.worker_processes || 1) }, [capabilities?.worker_processes])

  useEffect(() => {
    if (manifestInputs > 0) { setKind('load'); setCalls(manifestInputs); setLanguages(['en', 'id', 'zh']) }
  }, [manifestInputs])

  const overrides = useMemo(() => [
    'audio.realtime_pacing=true', `workers.processes=${processes}`, `audio.chunk_ms=${chunkMs}`,
    ...(!capabilities || capabilities.is_mock ? [] : [`model.${previewMode ? 'prefix_preview_ms' : 'decode_step_ms'}=${decodeMs}`]),
    ...extraOverrides.split('\n').map(item => item.trim()).filter(Boolean),
  ], [extraOverrides, processes, chunkMs, decodeMs, previewMode, capabilities])

  const request = (): SuiteRequest => {
    const axes = strategy === 'baseline' ? [] :
      [{ key: strategy === 'scale' ? 'concurrency' : axisKey,
        values: axisValues.split(',').map(value => value.trim()).filter(Boolean) }]
    return { kind, mode, calls, concurrency, warmups, repetitions, languages, overrides,
      ...(kind === 'sweep' ? { strategy, axes } : {}),
      ...(failureBound === '' ? {} : { max_failure_rate: Number(failureBound) }),
      ...(finalBound === '' ? {} : { max_p95_final_ms: Number(finalBound) }),
    }
  }

  async function resolve() {
    setBusy(true); setError('')
    try {
      const response = await requestJson<{ resolved: unknown }>(base, '/v1/config/resolve', { overrides })
      setResolved(response.resolved)
      log('info', 'experiment', 'Resolved configuration from the selected server YAML')
    } catch (cause) { setError(String(cause)) }
    finally { setBusy(false) }
  }

  async function dryRun() {
    setBusy(true); setError('')
    try {
      const response = await requestJson<unknown>(base, '/v1/suites/dry-run', request())
      setPlan(response)
      log('info', 'experiment', 'Preflight and effective cases generated without starting a model')
    } catch (cause) { setError(String(cause)) }
    finally { setBusy(false) }
  }

  async function start() {
    setBusy(true); setError('')
    try {
      const body = { ...request(), idempotency_key: `ui_${crypto.randomUUID()}` }
      const response = await requestJson<JobStatus>(base, '/v1/suites', body)
      setJob(response)
      log('info', 'experiment', `Started ${body.kind} job ${response.job_id}`)
    } catch (cause) { setError(String(cause)) }
    finally { setBusy(false) }
  }

  useEffect(() => {
    if (!job?.job_id || job.status !== 'RUNNING') return
    let active = true
    const poll = async () => {
      try {
        const latest = await requestJson<JobStatus>(base, `/v1/jobs/${job.job_id}`)
        if (!active) return
        setJob(latest)
        if (latest.status !== 'RUNNING') {
          log(latest.status === 'COMPLETE' ? 'info' : 'warn', 'experiment',
            `Job ${latest.job_id} finished: ${latest.status}`)
          onComplete()
        }
      } catch (cause) { if (active) setError(String(cause)) }
    }
    const timer = setInterval(() => { void poll() }, 1500)
    return () => { active = false; clearInterval(timer) }
  }, [base, job?.job_id, job?.status, log, onComplete])

  async function stop() {
    if (!job?.job_id) return
    try {
      await requestJson(base, `/v1/jobs/${job.job_id}/stop`, {})
      log('warn', 'experiment', capabilities?.hard_decode_watchdog
        ? 'Stop requested; active native call may continue to its watchdog'
        : 'Stop requested; active calls finish before remaining calls are stopped')
    } catch (cause) { setError(String(cause)) }
  }

  const result = asRecord(job?.result)
  const metrics = asRecord(result.metrics)
  const quality = summarizeAccuracy(callsFromSummary(result))
  const planRecord = asRecord(plan)
  const preflight = asRecord(planRecord.preflight)
  const cases = Array.isArray(planRecord.cases) ? planRecord.cases : []
  const plannedCount = cases.length || (Array.isArray(planRecord.calls) ? planRecord.calls.length : 0)
  const canStart = capabilities && !busy && job?.status !== 'RUNNING' && languages.length > 0

  return <section className="panel experiment-panel" aria-labelledby="experiment-heading">
    <div className="panel-head">
      <div><p className="eyebrow">02 / CONTROL PLANE</p><h2 id="experiment-heading">Experiment editor</h2></div>
      <span className="subtle-tag">same C++ runner as CLI</span>
    </div>
    {manifestInputs > 0 && <p className="fine-print">Dataset: {manifestInputs} configured WAV recordings. Calls cycle through recordings matching the selected languages; each call receives paced audio chunks.</p>}
    <div className="form-grid three">
      <label className="field">Suite
        <select aria-label="Suite" value={kind} onChange={event => setKind(event.target.value as 'load' | 'sweep')}>
          <option value="load">Load</option><option value="sweep" disabled={manifestInputs > 0}>Sweep</option>
        </select>
      </label>
      <label className="field">Ingress
        <select value={mode} onChange={event => setMode(event.target.value as 'direct' | 'network')}>
          <option value="direct">Direct</option><option value="network">WebSocket network</option>
        </select>
      </label>
      <label className="field">Worker processes
        <input type="number" min="1" max="16" value={processes} disabled={sharedModel} onChange={event => setProcesses(Number(event.target.value))} />
      </label>
      <label className="field">Suite chunk size
        <select value={chunkMs} onChange={event => setChunkMs(Number(event.target.value))}>
          {[50, 100, 200, 500, 1000].map(ms => <option key={ms} value={ms}>{ms} ms</option>)}
        </select>
      </label>
      <label className="field">{previewMode ? 'Suite prefix preview' : 'Suite decode step'}
        <select value={decodeMs} disabled={capabilities?.is_mock} onChange={event => setDecodeMs(Number(event.target.value))}>
          {(previewMode ? [1000, 2000, 4000, 8000, 12000, 20000] : [1000, 2000, 4000, 8000]).map(ms =>
            <option key={ms} value={ms}>{ms} ms</option>)}
        </select>
      </label>
      <label className="field">Calls per repetition
        <input type="number" min="1" max="1000" value={calls} onChange={event => setCalls(Number(event.target.value))} />
      </label>
      <label className="field">Concurrency
        <input type="number" min="1" max={activeLimit} value={concurrency} onChange={event => setConcurrency(Number(event.target.value))} />
      </label>
      <label className="field">Warmups
        <input type="number" min="0" max="20" value={warmups} onChange={event => setWarmups(Number(event.target.value))} />
      </label>
      <label className="field">Repetitions
        <input type="number" min="1" max="20" value={repetitions} onChange={event => setRepetitions(Number(event.target.value))} />
      </label>
      <label className="field">Max failure rate
        <input placeholder="optional, 0–1" value={failureBound} onChange={event => setFailureBound(event.target.value)} />
      </label>
      <label className="field">Max p95 final
        <input placeholder="optional, ms" value={finalBound} onChange={event => setFinalBound(event.target.value)} />
      </label>
    </div>
    <fieldset className="language-picks"><legend>Language assignment</legend>
      {(capabilities?.languages || ['en', 'id', 'zh']).map(value =>
        <label key={value}><input type="checkbox" checked={languages.includes(value as Language)}
          onChange={() => setLanguages(current => current.includes(value as Language)
            ? current.filter(item => item !== value) : [...current, value as Language])} />
          {value.toUpperCase()}</label>)}
    </fieldset>
    {kind === 'sweep' && <div className="form-grid three sweep-controls">
      <label className="field">Strategy
        <select value={strategy} onChange={event => setStrategy(event.target.value as typeof strategy)}>
          <option value="baseline">Baseline</option><option value="oat">One-factor</option>
          <option value="matrix">Matrix</option><option value="scale">Scale</option>
        </select>
      </label>
      {strategy !== 'baseline' && <><label className="field">Axis
        <select disabled={strategy === 'scale'} value={strategy === 'scale' ? 'concurrency' : axisKey}
          onChange={event => setAxisKey(event.target.value)}>
          {strategy === 'scale' ? <option value="concurrency">concurrency</option> : <>
            <option value="audio.chunk_ms">audio.chunk_ms</option>
            <option value="model.threads" disabled={capabilities?.is_mock || sharedModel}>model.threads</option>
            <option value="workers.processes" disabled={sharedModel}>workers.processes</option>
            <option value="model.decode_step_ms" disabled={capabilities?.is_mock || previewMode}>model.decode_step_ms</option>
          </>}
        </select>
      </label><label className="field">Values
        <input value={axisValues} onChange={event => setAxisValues(event.target.value)} placeholder="100,200,500" />
      </label></>}
    </div>}
    <label className="field overrides">Additional YAML overrides <span className="field-note">one `section.key=value` per line; model/audio/output paths are blocked by the API</span>
      <textarea rows={3} placeholder="audio.chunk_ms=200" value={extraOverrides}
        onChange={event => setExtraOverrides(event.target.value)} />
    </label>
    <div className="actions">
      <button onClick={() => void resolve()} disabled={!capabilities || busy}>Resolve config</button>
      <button onClick={() => void dryRun()} disabled={!capabilities || busy}>Dry run</button>
      <button className="primary" onClick={() => void start()} disabled={!canStart}>Start suite</button>
      <button onClick={() => void stop()} disabled={job?.status !== 'RUNNING'}>Stop job</button>
    </div>
    {error && <p role="alert" className="alert">{error}</p>}
    {plan !== null && <div className="info-block"><div className="subhead"><h3>Preflight</h3>
      <span className={planRecord.allowed === false ? 'danger-text' : 'success-text'}>
        {planRecord.allowed === false ? 'REJECTED' : `${plannedCount} planned`}
      </span></div>
      <p className="mono small">{JSON.stringify(preflight.skip_reasons || [])}</p>
      <details><summary>Effective plan JSON</summary><pre>{JSON.stringify(plan, null, 2)}</pre></details>
    </div>}
    {resolved !== null && <details className="info-block"><summary>Resolved configuration</summary><pre>{JSON.stringify(resolved, null, 2)}</pre></details>}
    {job && <div className="job-card"><div className="subhead"><h3>Latest job</h3><span className={`state-pill ${job.status.toLowerCase()}`}>{job.status}</span></div>
      <p className="mono small">{job.job_id}</p>
      {job.error && <p className="alert">{job.error}</p>}
      {job.result && <div className="stat-grid four">
        <div className="stat"><span>Completed calls</span><strong>{numberAt(result, 'completed_calls') ?? '—'}</strong></div>
        <div className="stat"><span>First text mean / p95</span><strong>{formatMs(numberAt(metrics, 'first_usable_ms', 'mean'))} / {formatMs(numberAt(metrics, 'first_usable_ms', 'p95'))}</strong></div>
        <div className="stat"><span>EOF to final mean / p95</span><strong>{formatMs(numberAt(metrics, 'finalization_ms', 'mean'))} / {formatMs(numberAt(metrics, 'finalization_ms', 'p95'))}</strong></div>
        <div className="stat"><span>p95 final</span><strong>{formatMs(numberAt(metrics, 'final_result_ms', 'p95'))}</strong></div>
        <div className="stat"><span>Peak tree RSS</span><strong>{formatBytes(numberAt(metrics, 'sampled_peak_tree_rss_bytes'))}</strong></div>
        <div className="stat"><span>SLO qualified</span><strong>{textAt(result, 'slo', 'qualified') || (asRecord(result.slo).qualified === true ? 'yes' : 'no')}</strong></div>
      </div>}
      {quality.length > 0 && <div className="stat-grid three">{quality.map(group => <div className="stat" key={group.language}>
        <span>{group.language.toUpperCase()} {group.metric} · completed calls</span>
        <strong>{group.rate === null ? '—' : `${(group.rate * 100).toFixed(2)}%`}</strong>
        <small>{group.scored}/{group.completed} completed calls scored; {group.offered - group.completed} failed/stopped</small>
      </div>)}</div>}
      {job.result && <p className="fine-print">Accuracy requires references in the server input manifest. Languages are scored separately; failures and calls without references are excluded from WER/CER.</p>}
    </div>}
    <p className="fine-print">Mock results are structural only. Fewer than 20 measured calls cannot qualify a tail estimate. A job can be stopped between calls; {capabilities?.hard_decode_watchdog ? 'active native decoding is watchdog-bounded.' : 'an active decode must finish before stop completes.'}</p>
  </section>
}
