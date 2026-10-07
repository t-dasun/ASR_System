import { socketUrl, type Language } from './api'
import { pcmSlice, type PcmWav } from './wav'

export type StreamState = 'idle' | 'connecting' | 'ready' | 'streaming' | 'finalizing' |
  'stopping' | 'completed' | 'stopped' | 'failed'

export interface TranscriptRevision {
  sequence: number
  revision: number
  kind: string
  text: string
  before_eof: boolean
  consumed_samples: number
  worker_id: string
}

export interface StreamSnapshot {
  state: StreamState
  runId: string
  callId: string
  language: Language
  workerId: string
  sentChunks: number
  ackedChunks: number
  totalChunks: number
  totalSamples: number
  clientFirstResultMs: number | null
  clientEofToFinalMs: number | null
  clientMaxSendLagMs: number
  clientBufferedBytes: number
  observerDropped: number
  transcript: string
  revisions: TranscriptRevision[]
  error: string
}

type Control = Record<string, unknown> & { type: string; v: number }
const terminal = (state: StreamState) => ['completed', 'stopped', 'failed', 'idle'].includes(state)

export class PacedStream {
  private audio: WebSocket | null = null
  private observer: WebSocket | null = null
  private timer: ReturnType<typeof setTimeout> | null = null
  private token = 0
  private readyAt = 0
  private eofAt: number | null = null
  private nextSequence = 0
  private credit = 0
  private observerCursor = 0
  private observerAttempts = 0
  private preparing = false
  private eofSent = false
  private seenRevisions = new Set<string>()
  private snapshot: StreamSnapshot

  constructor(private readonly base: string, private readonly file: Blob,
              private readonly wav: PcmWav, private readonly chunkMs: number,
              language: Language, private readonly onUpdate: (value: StreamSnapshot) => void,
              private readonly onLog: (level: string, message: string) => void,
              private readonly decode: { decode_step_ms?: number; prefix_preview_ms?: number } = {}) {
    const runId = `ui_${crypto.randomUUID()}`
    this.snapshot = { state: 'idle', runId, callId: `${runId}_call_0`, language,
      workerId: '', sentChunks: 0, ackedChunks: 0,
      totalChunks: Math.ceil(wav.samples / (chunkMs * 16)), totalSamples: wav.samples,
      clientFirstResultMs: null, clientEofToFinalMs: null, clientMaxSendLagMs: 0, clientBufferedBytes: 0,
      observerDropped: 0, transcript: '', revisions: [], error: '' }
  }

  get value(): StreamSnapshot { return this.snapshot }
  private update(changes: Partial<StreamSnapshot>): void {
    this.snapshot = { ...this.snapshot, ...changes }
    this.onUpdate(this.snapshot)
  }
  private fail(reason: string): void {
    if (this.snapshot.state === 'failed' || this.snapshot.state === 'completed') return
    this.onLog('error', reason)
    this.update({ state: 'failed', error: reason })
    if (this.timer) clearTimeout(this.timer)
    this.audio?.close()
    this.observer?.close()
  }

  start(): void {
    if (this.snapshot.state !== 'idle') throw new Error('Stream already started')
    this.update({ state: 'connecting' })
    const token = ++this.token
    const socket = new WebSocket(socketUrl(this.base, '/v1/asr'))
    this.audio = socket
    socket.binaryType = 'arraybuffer'
    socket.onopen = () => {
      if (token !== this.token) return
      this.openObserver(token)
      this.send({ v: 1, type: 'start', run_id: this.snapshot.runId,
        call_id: this.snapshot.callId, language: this.snapshot.language,
        seed: 42, max_chunk_samples: this.chunkMs * 16,
        partial_every_ms: 400, sample_rate_hz: 16000, ...this.decode })
    }
    socket.onmessage = (message) => {
      if (token !== this.token || typeof message.data !== 'string') return
      try { this.handleAudio(JSON.parse(message.data) as Control, token) }
      catch (error) { this.fail(error instanceof Error ? error.message : String(error)) }
    }
    socket.onerror = () => this.fail('Audio WebSocket failed')
    socket.onclose = () => {
      if (token === this.token && !terminal(this.snapshot.state)) {
        this.fail('Audio WebSocket closed before terminal status')
      }
    }
  }

  private send(control: Control): void {
    if (!this.audio || this.audio.readyState !== WebSocket.OPEN) {
      throw new Error('Audio socket is not open')
    }
    this.audio.send(JSON.stringify(control))
  }

  private handleAudio(message: Control, token: number): void {
    if (message.v !== 1) throw new Error('Unsupported ASR protocol version')
    if (message.type === 'ready') {
      const status = message.status as { code?: string; message?: string }
      if (status?.code !== 'none' || message.credits !== 1) {
        throw new Error(status?.message || 'Session was not admitted')
      }
      this.credit = 1
      this.readyAt = performance.now()
      this.update({ state: 'ready', workerId: String(message.worker_id || '') })
      this.schedule(token)
    } else if (message.type === 'ack') {
      const status = message.status as { code?: string; message?: string }
      if (status?.code !== 'none' || message.sequence !== this.snapshot.ackedChunks ||
          message.credits !== 1) {
        throw new Error(status?.message || 'Chunk ACK/credit mismatch')
      }
      this.credit = 1
      this.update({ ackedChunks: this.snapshot.ackedChunks + 1,
        clientBufferedBytes: this.audio?.bufferedAmount ?? 0 })
      this.schedule(token)
    } else if (message.type === 'event') {
      const event = message.event as TranscriptRevision
      this.acceptEvent(event)
    } else if (message.type === 'done') {
      const status = message.status as { code?: string; message?: string }
      if (status?.code === 'none') {
        this.update({ state: this.snapshot.state === 'stopping' ? 'stopped' : 'completed' })
      } else if (status?.code === 'cancelled') {
        this.update({ state: 'stopped' })
      } else {
        this.fail(status?.message || 'ASR session failed')
      }
    } else if (message.type === 'observation') {
      // Raw server timing is available on the observer feed; it is never
      // subtracted from performance.now() in this browser clock domain.
    } else {
      throw new Error(`Unknown ASR message: ${message.type}`)
    }
  }

  private acceptEvent(event: TranscriptRevision): void {
    const key = `${event.sequence}:${event.revision}`
    if (this.seenRevisions.has(key)) return
    this.seenRevisions.add(key)
    const arrival = performance.now()
    const latestSequence = this.snapshot.revisions.reduce((latest, revision) => Math.max(latest, revision.sequence), -1)
    this.update({
      clientFirstResultMs: this.snapshot.clientFirstResultMs ?? (event.text.trim() ? arrival - this.readyAt : null),
      clientEofToFinalMs: event.kind === 'final' && this.eofAt !== null
        ? arrival - this.eofAt : this.snapshot.clientEofToFinalMs,
      transcript: event.sequence >= latestSequence ? event.text || this.snapshot.transcript : this.snapshot.transcript,
      revisions: [...this.snapshot.revisions, event].sort((a, b) => a.sequence - b.sequence).slice(-200),
      workerId: event.worker_id || this.snapshot.workerId })
  }

  private openObserver(token: number): void {
    if (token !== this.token || this.observerAttempts >= 4) return
    const path = `/v1/observe?run_id=${this.snapshot.runId}&call_id=${this.snapshot.callId}` +
      `&since=${this.observerCursor}`
    const socket = new WebSocket(socketUrl(this.base, path))
    this.observer = socket
    socket.onmessage = (received) => {
      if (token !== this.token || typeof received.data !== 'string') return
      try {
        const message = JSON.parse(received.data) as Control & { delivery_sequence?: number }
        if (message.v !== 1) throw new Error('Observer protocol mismatch')
        if (message.type === 'event') this.acceptEvent(message.event as TranscriptRevision)
        else if (message.type === 'gap') {
          this.update({ observerDropped: Number(message.dropped_total || 0) })
          this.onLog('warn', 'Observer replay gap: older live messages were evicted')
        } else if (message.type === 'observation') {
          // Metrics are surfaced by /v1/runtime on an independent slow chart cadence.
        }
        if (typeof message.delivery_sequence === 'number') {
          this.observerCursor = message.delivery_sequence
        }
      } catch (error) { this.onLog('error', `Observer event rejected: ${String(error)}`) }
    }
    socket.onclose = () => {
      if (token !== this.token || terminal(this.snapshot.state)) return
      this.observerAttempts++
      setTimeout(() => this.openObserver(token), 500 * this.observerAttempts)
    }
  }

  private schedule(token: number): void {
    if (token !== this.token || this.credit !== 1 || this.preparing || this.eofSent ||
        this.snapshot.state === 'stopping') return
    if (this.nextSequence >= this.snapshot.totalChunks) {
      if (this.snapshot.ackedChunks === this.snapshot.totalChunks) this.finish()
      return
    }
    const deadline = this.readyAt + (this.nextSequence + 1) * this.chunkMs
    const wait = Math.max(0, deadline - performance.now())
    if (this.timer) clearTimeout(this.timer)
    this.timer = setTimeout(() => { void this.sendChunk(token, deadline) }, wait)
  }

  private async sendChunk(token: number, deadline: number): Promise<void> {
    if (token !== this.token || this.credit !== 1 || this.preparing || terminal(this.snapshot.state)) return
    this.preparing = true
    try {
      const sequence = this.nextSequence
      const firstSample = sequence * this.chunkMs * 16
      const sampleCount = Math.min(this.chunkMs * 16, this.wav.samples - firstSample)
      const pcm = await pcmSlice(this.file, this.wav, firstSample, sampleCount)
      if (token !== this.token || this.snapshot.state === 'stopping') return
      if ((this.audio?.bufferedAmount ?? 0) > 65536) {
        this.timer = setTimeout(() => { void this.sendChunk(token, deadline) }, 10)
        return
      }
      const lag = Math.max(0, performance.now() - deadline)
      this.send({ v: 1, type: 'chunk', sequence, first_sample: firstSample,
        sample_count: sampleCount, scheduled_ready_ns: 0, sent_ns: 0 })
      this.audio!.send(pcm)
      this.credit = 0
      this.nextSequence++
      this.update({ state: 'streaming', sentChunks: this.nextSequence,
        clientMaxSendLagMs: Math.max(this.snapshot.clientMaxSendLagMs, lag),
        clientBufferedBytes: this.audio!.bufferedAmount })
    } catch (error) {
      this.fail(error instanceof Error ? error.message : String(error))
    } finally {
      this.preparing = false
    }
  }

  private finish(): void {
    if (this.eofSent) return
    this.eofSent = true
    this.eofAt = performance.now()
    this.send({ v: 1, type: 'eof', expected_next_sequence: this.nextSequence,
      total_samples: this.wav.samples })
    this.update({ state: 'finalizing' })
  }

  stop(): void {
    if (terminal(this.snapshot.state) || this.snapshot.state === 'stopping') return
    if (this.timer) clearTimeout(this.timer)
    if (this.audio?.readyState === WebSocket.OPEN) {
      this.send({ v: 1, type: 'cancel' })
      this.update({ state: 'stopping' })
    } else {
      this.audio?.close()
      this.update({ state: 'stopped' })
    }
  }

  dispose(): void {
    ++this.token
    if (this.timer) clearTimeout(this.timer)
    this.audio?.close()
    this.observer?.close()
  }
}
