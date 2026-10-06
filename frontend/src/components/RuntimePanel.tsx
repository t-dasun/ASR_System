import { useEffect, useRef, useState } from 'react'
import { requestJson, type RuntimeSnapshot } from '../api'
import { formatBytes, sparklinePoints } from '../metrics'
import type { LogWriter } from '../types'

interface Props { base: string; connected: boolean; log: LogWriter }

export function RuntimePanel({ base, connected, log }: Props) {
  const [runtime, setRuntime] = useState<RuntimeSnapshot | null>(null)
  const [chart, setChart] = useState<{ cpu: number[]; rss: number[] }>({ cpu: [], rss: [] })
  const [error, setError] = useState('')
  const pending = useRef<{ cpu: number[]; rss: number[] }>({ cpu: [], rss: [] })

  useEffect(() => {
    if (!connected) { setRuntime(null); return }
    let active = true
    let reported = false
    const sample = async () => {
      try {
        const latest = await requestJson<RuntimeSnapshot>(base, '/v1/runtime')
        if (!active) return
        setRuntime(latest)
        setError('')
        reported = false
        pending.current.cpu.push(latest.system?.host_cpu_percent ?? 0)
        pending.current.rss.push(latest.system?.sampled_process_tree_rss_bytes ?? 0)
        pending.current.cpu = pending.current.cpu.slice(-30)
        pending.current.rss = pending.current.rss.slice(-30)
      } catch (cause) {
        if (active && !reported) {
          reported = true
          setError(String(cause))
          log('warn', 'runtime', `Snapshot unavailable: ${String(cause)}`)
        }
      }
    }
    void sample()
    const sampling = setInterval(() => { void sample() }, 2000)
    // Rendering the chart is deliberately slower than server telemetry reads.
    const chartRefresh = setInterval(() => setChart({
      cpu: [...pending.current.cpu], rss: [...pending.current.rss],
    }), 5000)
    return () => { active = false; clearInterval(sampling); clearInterval(chartRefresh) }
  }, [base, connected, log])

  const cpu = runtime?.system?.host_cpu_percent
  const memoryTotal = runtime?.system?.host_memory_total_bytes || 0
  const memoryAvailable = runtime?.system?.host_memory_available_bytes || 0
  const used = memoryTotal ? Math.max(0, memoryTotal - memoryAvailable) : null
  return <section className="panel runtime-panel" aria-labelledby="runtime-heading">
    <div className="panel-head"><div><p className="eyebrow">03 / OBSERVABILITY</p><h2 id="runtime-heading">Live runtime</h2></div>
      <span className="subtle-tag">2 s sample · 5 s chart</span></div>
    {error && <p className="warning">{error}</p>}
    <div className="stat-grid four">
      <div className="stat"><span>Host CPU</span><strong>{cpu === null || cpu === undefined ? '—' : `${cpu.toFixed(1)}%`}</strong></div>
      <div className="stat"><span>Host RAM used</span><strong>{formatBytes(used)}</strong></div>
      <div className="stat"><span>Process-tree RSS</span><strong>{formatBytes(runtime?.system?.sampled_process_tree_rss_bytes)}</strong></div>
      <div className="stat"><span>Active workers</span><strong>{runtime?.workers.filter(item => item.occupied).length ?? '—'}</strong></div>
    </div>
    <div className="chart-grid">
      <div className="mini-chart"><div className="subhead"><h3>Host CPU trend</h3><span>%</span></div>
        <svg viewBox="0 0 240 58" preserveAspectRatio="none" role="img" aria-label="Host CPU recent trend">
          <polyline points={sparklinePoints(chart.cpu)} /></svg></div>
      <div className="mini-chart"><div className="subhead"><h3>Process-tree RSS trend</h3><span>bytes</span></div>
        <svg viewBox="0 0 240 58" preserveAspectRatio="none" role="img" aria-label="Process RSS recent trend">
          <polyline points={sparklinePoints(chart.rss)} /></svg></div>
    </div>
    <div className="subhead workers-title"><h3>Interactive worker slots</h3><span>{runtime?.workers.length ?? 0} configured</span></div>
    <div className="table-wrap"><table><thead><tr><th>Worker</th><th>PID</th><th>Sessions</th><th>State</th><th>Call</th><th>Lang</th><th>Threads</th><th>Queue</th></tr></thead>
      <tbody>{runtime?.workers.length ? runtime.workers.map(worker => <tr key={worker.worker_id}>
        <td className="mono">{worker.worker_id}</td><td>{worker.process_id}</td><td>{worker.active_sessions}{worker.max_sessions ? ` / ${worker.max_sessions}` : ''}</td><td><span className={`dot ${worker.occupied ? 'live' : ''}`} />{worker.healthy ? worker.state : 'failed'}</td>
        <td className="mono clip-cell" title={worker.calls?.map(call => call.call_id).join(', ') || worker.call_id}>
          {worker.calls?.length ? `${worker.active_sessions} calls: ${worker.calls.map(call => call.call_id).join(', ')}` : worker.call_id || '—'}</td><td>{worker.calls?.length ? [...new Set(worker.calls.map(call => call.language))].join(', ') : worker.language || '—'}</td>
        <td>{worker.runtime_threads}</td><td>{worker.server_queue_depth ?? 'not exposed'}</td>
      </tr>) : <tr><td colSpan={8} className="empty-cell">Connect to a service for worker state.</td></tr>}</tbody></table></div>
    <p className="fine-print">This snapshot samples active calls and the host process tree. Benchmark distributions appear after a job completes. {runtime?.system?.queue_depth_available ? 'Queue shows decode jobs waiting for the shared model.' : 'Server queue depth is unavailable in this runtime.'}</p>
  </section>
}
