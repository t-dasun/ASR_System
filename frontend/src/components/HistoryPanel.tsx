import { useCallback, useEffect, useMemo, useState } from 'react'
import { downloadArtifact, requestJson, type HistoryItem } from '../api'
import { asRecord, callsFromSummary, formatMs, numberAt, textAt, type CallRecord } from '../metrics'
import type { LogWriter } from '../types'

interface Props { base: string; connected: boolean; refresh: number; log: LogWriter }
const files = ['summary.json', 'status.json', 'plan.json', 'config.json', 'environment.json',
  'events.jsonl', 'audio_timing.jsonl', 'runtime_timing.jsonl', 'system_metrics.jsonl',
  'calls.jsonl', 'workers.jsonl', 'errors.jsonl']

export function HistoryPanel({ base, connected, refresh, log }: Props) {
  const [items, setItems] = useState<HistoryItem[]>([])
  const [left, setLeft] = useState('')
  const [right, setRight] = useState('')
  const [summaries, setSummaries] = useState<Record<string, unknown>>({})
  const [error, setError] = useState('')

  const load = useCallback(async () => {
    if (!connected) return
    try {
      const data = await requestJson<{ items: HistoryItem[] }>(base, '/v1/history')
      setItems(data.items || [])
      setError('')
    } catch (cause) { setError(String(cause)) }
  }, [base, connected])
  useEffect(() => { void load() }, [load, refresh])
  useEffect(() => { setSummaries({}); setLeft(''); setRight('') }, [base])
  useEffect(() => {
    if (!connected) return
    let active = true
    for (const id of new Set([left, right].filter(Boolean))) {
      if (id in summaries) continue
      void requestJson<unknown>(base, `/v1/history/${id}`).then(summary => {
        if (active) setSummaries(current => ({ ...current, [id]: summary }))
      }).catch(cause => { if (active) setError(String(cause)) })
    }
    return () => { active = false }
  }, [base, connected, left, right, summaries])

  const first = summaries[left]
  const second = summaries[right]
  const callsA = useMemo(() => callsFromSummary(first), [first])
  const callsB = useMemo(() => callsFromSummary(second), [second])
  async function artifact(id: string, name: string, call?: CallRecord) {
    try { await downloadArtifact(base, id, name, call?.artifactId || undefined) }
    catch (cause) { setError(String(cause)); log('warn', 'history', `Download unavailable: ${name}`) }
  }

  function runCard(id: string, summary: unknown, calls: CallRecord[]) {
    const root = asRecord(summary)
    const metrics = asRecord(root.metrics)
    return <div className="run-card">
      <div className="subhead"><h3 className="mono">{id || 'Select a run'}</h3>
        <span className={`state-pill ${textAt(root, 'status').toLowerCase()}`}>{textAt(root, 'status') || '—'}</span></div>
      {id && !summary && <p className="muted">Loading summary…</p>}
      {summary !== undefined && <>
        <div className="stat-grid three">
          <div className="stat"><span>Calls</span><strong>{numberAt(root, 'completed_calls') ?? calls.length}</strong></div>
          <div className="stat"><span>p50 final</span><strong>{formatMs(numberAt(metrics, 'final_result_ms', 'p50'))}</strong></div>
          <div className="stat"><span>p95 final</span><strong>{formatMs(numberAt(metrics, 'final_result_ms', 'p95'))}</strong></div>
        </div>
        <p className="fine-print">{root.is_mock === true ? 'Mock engine · structural result' : 'Native engine'} · {textAt(root, 'mode') || 'direct'} mode</p>
        <div className="call-list">{calls.slice(0, 30).map((call, index) => <div className="call-row" key={`${call.label}-${index}`}>
          <div className="subhead"><span className="mono">{call.label || `call ${index + 1}`}</span>
            <span>{call.language.toUpperCase()} · {formatMs(call.finalMs)}</span></div>
          <p>{call.transcript || <em>No transcript recorded</em>}</p>
          <small>{call.status} · {call.worker || 'worker unavailable'}</small>
          {call.artifactId && <button className="text-button" onClick={() => void artifact(id, 'events.jsonl', call)}>events.jsonl ↓</button>}
        </div>)}</div>
        {calls.length > 30 && <p className="fine-print">Showing first 30 of {calls.length} calls. Download raw artifacts for the full run.</p>}
        <div className="artifact-grid">{files.map(name => <button key={name} onClick={() => void artifact(id, name)}>{name} ↓</button>)}</div>
      </>}
    </div>
  }

  return <section className="panel history-panel" aria-labelledby="history-heading">
    <div className="panel-head"><div><p className="eyebrow">04 / EVIDENCE</p><h2 id="history-heading">Run history & comparison</h2></div>
      <button onClick={() => void load()} disabled={!connected}>Refresh runs</button></div>
    {error && <p role="alert" className="alert">{error}</p>}
    <div className="form-grid two">
      <label className="field">Run A<select value={left} onChange={event => setLeft(event.target.value)}><option value="">Select run</option>
        {items.map(item => <option key={item.id} value={item.id}>{item.id} · {item.status?.status || 'unknown'}</option>)}</select></label>
      <label className="field">Run B<select value={right} onChange={event => setRight(event.target.value)}><option value="">Select run</option>
        {items.map(item => <option key={item.id} value={item.id}>{item.id} · {item.status?.status || 'unknown'}</option>)}</select></label>
    </div>
    <div className="comparison-grid">{runCard(left, first, callsA)}{runCard(right, second, callsB)}</div>
    <p className="fine-print">Transcripts are shown side by side; timing summaries are server measurements. Artifact buttons request allow-listed files and may report unavailable when a run did not produce that file.</p>
  </section>
}
