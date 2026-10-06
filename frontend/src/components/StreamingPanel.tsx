import { useEffect, useMemo, useRef, useState } from 'react'
import type { Capabilities, Language } from '../api'
import { scoreTranscript } from '../accuracy'
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
  const [decodeMs, setDecodeMs] = useState(4000)
  const [reference, setReference] = useState('')
  const sharedModel = capabilities?.engine.startsWith('qwen_prefix_') || false
  useEffect(() => {
    setChunkMs(capabilities?.chunk_ms || 200)
    setDecodeMs(sharedModel ? capabilities?.prefix_preview_ms || 4000 : capabilities?.decode_step_ms || 2000)
  }, [capabilities?.chunk_ms, capabilities?.prefix_preview_ms, capabilities?.decode_step_ms, sharedModel])
  const [snapshot, setSnapshot] = useState<StreamSnapshot | null>(null)
  const [error, setError] = useState('')
  const stream = useRef<PacedStream | null>(null)

  useEffect(() => {
    stream.current?.dispose(); stream.current = null; setSnapshot(null)
    return () => stream.current?.dispose()
  }, [base])

  async function chooseFile(selected: File | null) {
    stream.current?.dispose()
    stream.current = null
    setSnapshot(null)
    setFile(selected)
    setReference('')
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
      (level, message) => log(level as 'info' | 'warn' | 'error', 'stream', message),
      capabilities.session_decode_controls && !capabilities.is_mock
        ? sharedModel ? { prefix_preview_ms: decodeMs } : { decode_step_ms: decodeMs } : {})
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

  const accuracy = useMemo(() => {
    if (snapshot?.state !== 'completed' || !reference.trim()) return { value: null, error: '' }
    try { return { value: scoreTranscript(reference, snapshot.transcript, snapshot.language), error: '' } }
    catch (cause) { return { value: null, error: String(cause) } }
  }, [reference, snapshot?.state, snapshot?.transcript, snapshot?.language])

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
          {[50, 100, 200, 500, 1000].map(ms => <option key={ms} value={ms}>{ms} ms</option>)}
        </select>
      </label>
      <label className="field">{sharedModel ? 'Prefix preview interval' : 'Decode step'}
        <select value={decodeMs} disabled={Boolean(active) || !capabilities?.session_decode_controls || capabilities?.is_mock}
          onChange={event => setDecodeMs(Number(event.target.value))}>
          {(sharedModel ? [1000, 2000, 4000, 8000, 12000, 20000] : [1000, 2000, 4000, 8000]).map(ms =>
            <option key={ms} value={ms}>{ms} ms</option>)}
        </select>
        <span className="field-note">{sharedModel ? 'One preview after this much audio, then final at EOF.' : 'Native progressive decode interval.'}</span>
      </label>
    </div>
    <label className="field">Reference transcript (optional)
      <textarea rows={3} maxLength={8000} value={reference} onChange={event => setReference(event.target.value)}
        placeholder="Paste the correct transcript to calculate WER/CER after a successful final result." />
    </label>
    <div className="actions">
      <button className="primary" disabled={!capabilities || !wav || Boolean(active)} onClick={start}>Start stream</button>
      <button disabled={!active || snapshot?.state === 'finalizing'} onClick={() => stream.current?.stop()}>Stop</button>
      <button onClick={reset}>Reset</button>
    </div>
    {(error || snapshot?.error) && <p className="alert" role="alert">{error || snapshot?.error}</p>}
    <div className="progress-label"><span>ACKed chunks</span><strong>{snapshot?.ackedChunks || 0} / {snapshot?.totalChunks || 0}</strong></div>
    <div className="progress-track"><div style={{ width: `${progress}%` }} /></div>
    <div className="stat-grid four">
      <div className="stat"><span>First text arrival</span><strong>{formatMs(snapshot?.clientFirstResultMs)}</strong></div>
      <div className="stat"><span>EOF to final text</span><strong>{formatMs(snapshot?.clientEofToFinalMs)}</strong></div>
      <div className="stat"><span>{snapshot?.language === 'zh' || (!snapshot && language === 'zh') ? 'CER' : 'WER'} (completed call)</span><strong>{accuracy.value ? `${(accuracy.value.rate * 100).toFixed(2)}%` : '—'}</strong></div>
      <div className="stat"><span>Max client send lag</span><strong>{formatMs(snapshot?.clientMaxSendLagMs)}</strong></div>
      <div className="stat"><span>Browser socket buffer</span><strong>{snapshot?.clientBufferedBytes || 0} B</strong></div>
      <div className="stat"><span>Worker</span><strong>{snapshot?.workerId || '—'}</strong></div>
    </div>
    {accuracy.error && <p className="alert">{accuracy.error}</p>}
    <p className="fine-print">{accuracy.value ? `${accuracy.value.edits} edits / ${accuracy.value.referenceUnits} reference units. ` : ''}WER/CER requires a reference and a completed final result. Failed or cancelled calls are not scored here.</p>
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
    <p className="fine-print">Arrival delays use this browser’s clock: first text from session ready; EOF delay from sending EOF to receiving final text. They include transport delay and differ from worker-side timings shown in experiments.</p>
  </section>
}
