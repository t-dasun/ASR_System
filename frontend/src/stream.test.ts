import { afterEach, expect, it, vi } from 'vitest'
import { PacedStream } from './stream'

class Socket {
  static OPEN = 1
  static sockets: Socket[] = []
  readyState = 1
  bufferedAmount = 0
  onopen?: () => void
  onmessage?: (event: { data: string }) => void
  sent: unknown[] = []
  constructor(_url: string) { Socket.sockets.push(this) }
  send(value: unknown) { this.sent.push(value) }
  close() { this.readyState = 3 }
  message(value: unknown) { this.onmessage?.({ data: JSON.stringify(value) }) }
}

afterEach(() => { vi.useRealTimers(); vi.unstubAllGlobals(); Socket.sockets = [] })

it('sends decode controls and measures earliest text/final arrival without duplicate revisions', async () => {
  vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'performance'] })
  vi.stubGlobal('WebSocket', Socket)
  const stream = new PacedStream('http://127.0.0.1:8080', new Blob([new Uint8Array(3200)]),
    { dataOffset: 0, samples: 1600, durationSeconds: .1, sampleRate: 16000, channels: 1, bitsPerSample: 16 },
    100, 'en', () => {}, () => {}, { prefix_preview_ms: 2000 })
  stream.start()
  const audio = Socket.sockets[0]
  audio.onopen?.()
  expect(JSON.parse(audio.sent[0] as string).prefix_preview_ms).toBe(2000)
  audio.message({ v: 1, type: 'ready', status: { code: 'none' }, credits: 1 })
  await vi.advanceTimersByTimeAsync(50)
  const partial = { sequence: 0, revision: 1, kind: 'partial', text: 'hello', worker_id: 'w0' }
  Socket.sockets[1].message({ v: 1, type: 'event', event: partial })
  audio.message({ v: 1, type: 'event', event: partial })
  expect(stream.value.clientFirstResultMs).toBe(50)
  expect(stream.value.revisions).toHaveLength(1)
  await vi.advanceTimersByTimeAsync(50)
  audio.message({ v: 1, type: 'ack', sequence: 0, credits: 1, status: { code: 'none' } })
  expect(stream.value.state).toBe('finalizing')
  await vi.advanceTimersByTimeAsync(25)
  audio.message({ v: 1, type: 'event', event: { ...partial, sequence: 1, revision: 2, kind: 'final' } })
  audio.message({ v: 1, type: 'done', status: { code: 'none' } })
  expect(stream.value.state).toBe('completed')
  expect(stream.value.clientEofToFinalMs).toBe(25)
  expect(stream.value.revisions).toHaveLength(2)
  stream.dispose()
})
