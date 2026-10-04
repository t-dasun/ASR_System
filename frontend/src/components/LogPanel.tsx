import { useMemo, useState } from 'react'
import type { LogEntry } from '../types'

export function LogPanel({ entries }: { entries: LogEntry[] }) {
  const [level, setLevel] = useState('all')
  const [source, setSource] = useState('all')
  const [query, setQuery] = useState('')
  const sources = useMemo(() => [...new Set(entries.map(item => item.source))].sort(), [entries])
  const visible = useMemo(() => entries.filter(item =>
    (level === 'all' || item.level === level) && (source === 'all' || item.source === source) &&
    item.message.toLowerCase().includes(query.toLowerCase())).slice(-100).reverse(), [entries, level, source, query])
  return <section className="panel log-panel" aria-labelledby="log-heading">
    <div className="panel-head"><div><p className="eyebrow">05 / EVENT CONSOLE</p><h2 id="log-heading">Structured logs</h2></div>
      <span className="subtle-tag">latest 100 visible</span></div>
    <div className="form-grid three">
      <label className="field">Level<select value={level} onChange={event => setLevel(event.target.value)}>
        {['all', 'info', 'warn', 'error'].map(item => <option key={item}>{item}</option>)}</select></label>
      <label className="field">Source<select value={source} onChange={event => setSource(event.target.value)}><option>all</option>
        {sources.map(item => <option key={item}>{item}</option>)}</select></label>
      <label className="field">Search<input value={query} onChange={event => setQuery(event.target.value)} placeholder="message text" /></label>
    </div>
    <div className="log-list" role="log" aria-live="off">{visible.length ? visible.map(item => <div className={`log-entry ${item.level}`} key={item.id}>
      <time>{item.at}</time><span className="mono">{item.level.toUpperCase()}</span><span>{item.source}</span><p>{item.message}</p>
    </div>) : <p className="empty-cell">No matching dashboard events.</p>}</div>
  </section>
}
