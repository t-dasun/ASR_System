import { useEffect, useRef, useState } from 'react'
import type { Capabilities, Language } from '../api'
import { formatMs } from '../metrics'
import { PacedStream, type StreamSnapshot } from '../stream'
import type { LogWriter } from '../types'
import { parsePcmWav, type PcmWav } from '../wav'

interface Props { base: string; capabilities: Capabilities | null; log: LogWriter }
const labels: Record<Language, string> = { en: 'English', id: 'Indonesian', zh: 'Mandarin' }

export function StreamingPanel({ base, capabilities, log }: Props) {
  const [file, setFile] = useState<File | null>(null)
  const [wav, setWav] = useState<PcmWav | null>(null)
  const [language, setLanguage] = useState<Language>('en')
  const [chunkMs, setChunkMs] = useState(200)
  const [snapshot, setSnapshot] = useState<StreamSnapshot | null>(null)
  const [error, setError] = useState('')
  const stream = useRef<PacedStream | null>(null)

  useEffect(() => () => stream.current?.dispose(), [])

  async function chooseFile(selected: File | null) {
    stream.current?.dispose()
    stream.current = null
    setSnapshot(null)
    setFile(selected)
    setWav(null)
    setError('')
    if (!selected) return
    try {
      const parsed = await parsePcmWav(selected)
      setWav(parsed)
      log('info', 'stream', `Validated ${selected.name}: ${parsed.durationSeconds.toFixed(2)} s PCM16`)
    } catch (cause) {
      const message = cause instanceof Error ? cause.message : String(cause)
      setError(message)
      log('error', 'stream', message)
    }
  }

  function start() {
    if (!capabilities || !file || !wav) return
    stream.current?.dispose()
    const controller = new PacedStream(base, file, wav, chunkMs, language, setSnapshot,
      (level, message) => log(level as 'info' | 'warn' | 'error', 'stream', message))
    stream.current = controller
    setSnapshot(controller.value)
    try {
      controller.start()
      log('info', 'stream', `Starting ${language} call at ${chunkMs} ms chunks`)
    } catch (cause) {
      const message = cause instanceof Error ? cause.message : String(cause)
      setError(message)
    }
  }

  function reset() {
    stream.current?.dispose()
    stream.current = null
    setSnapshot(null)
    setError('')
    log('info', 'stream', 'Session reset; next start receives a fresh call ID')
  }

  const active = snapshot && !['idle', 'completed', 'stopped', 'failed'].includes(snapshot.state)
  const progress = snapshot && snapshot.totalChunks
    ? Math.round(snapshot.ackedChunks / snapshot.totalChunks * 100) : 0
  return <section className="panel streaming-panel" aria-labelledby="stream-heading">
    <div className="panel-head">
      <div><p className="eyebrow">01 / LIVE INGRESS</p><h2 id="stream-heading">Streaming call</h2></div>
      <span className={`state-pill ${snapshot?.state || 'idle'}`}>{snapshot?.state || 'idle'}</span>
    </div>
    <p className="muted">A browser-paced file stream. Only 16 kHz mono PCM16 WAV is accepted; audio is sent in bounded slices, never uploaded whole.</p>
    <div className="form-grid">
      <label className="field file-field">Recording
        <input type="file" accept=".wav,audio/wav" onChange={event => void chooseFile(event.target.files?.[0] || null)} />
        <span className="field-note">{wav ? `${wav.durationSeconds.toFixed(2)} s · ${wav.samples.toLocaleString()} samples` : 'Choose a compatible WAV'}</span>
      </label>
      <label className="field">Language
        <select value={language} disabled={Boolean(active)} onChange={event => setLanguage(event.target.value as Language)}>
          {(capabilities?.languages || ['en', 'id', 'zh']).map(item =>
            <option key={item} value={item}>{labels[item as Language] || item}</option>)}
        </select>
      </label>
      <label className="field">Transport chunk
        <select value={chunkMs} disabled={Boolean(active)} onChange={event => setChunkMs(Number(event.target.value))}>
          {[100, 200, 500, 1000].map(ms => <option key={ms} value={ms}>{ms} ms</option>)}
        </select>
      </label>
    </div>
    <div className="actions">
      <button className="primary" disabled={!capabilities || !wav || Boolean(active)} onClick={start}>Start stream</button>
      <button disabled={!active || snapshot?.state === 'finalizing'} onClick={() => stream.current?.stop()}>Stop</button>
      <button onClick={reset}>Reset</button>
    </div>
    {(error || snapshot?.error) && <p className="alert" role="alert">{error || snapshot?.error}</p>}
    <div className="progress-label"><span>ACKed chunks</span><strong>{snapshot?.ackedChunks || 0} / {snapshot?.totalChunks || 0}</strong></div>
    <div className="progress-track"><div style={{ width: `${progress}%` }} /></div>
    <div className="stat-grid four">
      <div className="stat"><span>Client first text</span><strong>{formatMs(snapshot?.clientFirstResultMs)}</strong></div>
      <div className="stat"><span>Max client send lag</span><strong>{formatMs(snapshot?.clientMaxSendLagMs)}</strong></div>
      <div className="stat"><span>Browser socket buffer</span><strong>{snapshot?.clientBufferedBytes || 0} B</strong></div>
      <div className="stat"><span>Worker</span><strong>{snapshot?.workerId || '—'}</strong></div>
    </div>
    <div className="transcript-box">
      <div className="subhead"><h3>Transcript</h3><span>{snapshot?.revisions.length || 0} revisions</span></div>
      <p className={snapshot?.transcript ? '' : 'placeholder'}>{snapshot?.transcript || 'Waiting for a live transcript…'}</p>
      {snapshot?.observerDropped ? <small className="warning">Observer dropped {snapshot.observerDropped} older messages</small> : null}
    </div>
    {snapshot?.revisions.length ? <details className="revision-list"><summary>Revision timeline</summary>
      <ol>{snapshot.revisions.map((event, index) => <li key={`${event.sequence}-${index}`}>
        <span>{event.kind} · r{event.revision} · {event.consumed_samples} samples</span>
        <p>{event.text || '∅'}</p>
      </li>)}</ol>
    </details> : null}
    <p className="fine-print">Client timing uses this browser’s clock. Server latency is shown separately in experiment results; the two clocks are never subtracted.</p>
  </section>
}
