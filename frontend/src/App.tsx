import { useCallback, useEffect, useState } from 'react'
import { normalizeBase, requestJson, type Capabilities } from './api'
import { ExperimentPanel } from './components/ExperimentPanel'
import { HistoryPanel } from './components/HistoryPanel'
import { LogPanel } from './components/LogPanel'
import { RuntimePanel } from './components/RuntimePanel'
import { StreamingPanel } from './components/StreamingPanel'
import type { LogEntry, LogWriter } from './types'

const initialBase = () => {
  try { return normalizeBase(localStorage.getItem('asr-service-origin') || 'http://127.0.0.1:8765') }
  catch { return 'http://127.0.0.1:8765' }
}

export default function App() {
  const [address, setAddress] = useState(initialBase)
  const [base, setBase] = useState(initialBase)
  const [capabilities, setCapabilities] = useState<Capabilities | null>(null)
  const [connectionError, setConnectionError] = useState('')
  const [logs, setLogs] = useState<LogEntry[]>([])
  const [historyRefresh, setHistoryRefresh] = useState(0)
  const log: LogWriter = useCallback((level, source, message) => {
    setLogs(current => [...current.slice(-499), { id: Date.now() + Math.random(),
      at: new Date().toLocaleTimeString(), level, source, message }])
  }, [])
  const completed = useCallback(() => setHistoryRefresh(count => count + 1), [])

  useEffect(() => {
    let active = true
    requestJson<Capabilities>(base, '/v1/capabilities').then(data => {
      if (!active) return
      setCapabilities(data); setConnectionError('')
      log('info', 'service', `Connected to ${data.engine} (${data.device}, ${data.precision})`)
    }).catch(cause => {
      if (!active) return
      setCapabilities(null)
      setConnectionError(String(cause))
    })
    return () => { active = false }
  }, [base, log])

  function connect() {
    try {
      const next = normalizeBase(address)
      localStorage.setItem('asr-service-origin', next)
      if (next === base) {
        setCapabilities(null)
        void requestJson<Capabilities>(next, '/v1/capabilities').then(data => {
          setCapabilities(data); setConnectionError(''); log('info', 'service', 'Reconnected')
        }).catch(cause => setConnectionError(String(cause)))
      } else { setBase(next); setCapabilities(null) }
    } catch (cause) { setConnectionError(String(cause)) }
  }

  return <div className="app-shell">
    <header className="topbar"><div className="brand"><span className="brand-mark">⌁</span><span>ASR <b>SYSTEMS LAB</b></span></div>
      <div className="topbar-right"><span className="topbar-note">QWEN3-ASR / ENGINEERING CONSOLE</span>
        <span className={`connection ${capabilities ? 'online' : 'offline'}`}><span className="dot" />{capabilities ? 'SERVICE ONLINE' : 'OFFLINE'}</span></div></header>
    <main>
      <div className="hero"><div><p className="eyebrow">REAL-TIME SPEECH / PERFORMANCE WORKBENCH</p>
        <h1>Observe every<br/><em>millisecond.</em></h1><p>Stream speech, run controlled experiments, and inspect evidence from one local console.</p></div>
        <div className="hero-meta"><span>LOCAL API</span><strong>{base}</strong><span>MODEL</span><strong>{capabilities?.engine || 'not connected'}</strong>
          <span>DEVICE</span><strong>{capabilities?.device || '—'}</strong></div></div>
      <section className="connection-panel" aria-label="Service connection">
        <div><span className="eyebrow">SERVICE ENDPOINT</span><p>Connect to the loopback M7 service. Nothing is sent to a remote host.</p></div>
        <div className="connection-form"><input aria-label="Service origin" value={address} onChange={event => setAddress(event.target.value)}
          onKeyDown={event => { if (event.key === 'Enter') connect() }} /><button onClick={connect}>Connect ↗</button></div>
        {connectionError && <p className="connection-error" role="alert">{connectionError}</p>}
      </section>
      <div className="main-grid"><StreamingPanel base={base} capabilities={capabilities} log={log} />
        <RuntimePanel base={base} connected={Boolean(capabilities)} log={log} /></div>
      <ExperimentPanel base={base} capabilities={capabilities} log={log} onComplete={completed} />
      <HistoryPanel base={base} connected={Boolean(capabilities)} refresh={historyRefresh} log={log} />
      <LogPanel entries={logs} />
    </main><footer><span>ASR SYSTEMS LAB · LOCAL ENGINEERING DASHBOARD</span><span>Client observations ≠ server timings</span></footer>
  </div>
}
