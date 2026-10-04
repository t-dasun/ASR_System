import { describe, expect, it } from 'vitest'
import { normalizeBase } from './api'
import { callsFromSummary, formatBytes, formatMs, sparklinePoints } from './metrics'
import { parsePcmWav, pcmSlice } from './wav'

function wav(samples: number[], rate = 16000): Blob {
  const bytes = new ArrayBuffer(44 + samples.length * 2)
  const view = new DataView(bytes)
  for (const [offset, text] of [[0, 'RIFF'], [8, 'WAVE'], [12, 'fmt '], [36, 'data']] as const)
    for (let i = 0; i < text.length; i++) view.setUint8(offset + i, text.charCodeAt(i))
  view.setUint32(4, bytes.byteLength - 8, true)
  view.setUint32(16, 16, true)
  view.setUint16(20, 1, true)
  view.setUint16(22, 1, true)
  view.setUint32(24, rate, true)
  view.setUint32(28, rate * 2, true)
  view.setUint16(32, 2, true)
  view.setUint16(34, 16, true)
  view.setUint32(40, samples.length * 2, true)
  samples.forEach((sample, index) => view.setInt16(44 + index * 2, sample, true))
  return new Blob([bytes])
}

describe('local service boundary', () => {
  it('accepts only explicit loopback HTTP origins', () => {
    expect(normalizeBase('http://127.0.0.1:8765')).toBe('http://127.0.0.1:8765')
    for (const input of ['https://127.0.0.1:8765', 'http://example.com:8765',
      'http://localhost:8765/path', 'http://localhost:8765?x=1', 'http://localhost']) {
      expect(() => normalizeBase(input)).toThrow()
    }
  })
})

describe('bounded WAV reader', () => {
  it('validates the header and slices only requested PCM', async () => {
    const file = wav([1, -2, 3, -4])
    const parsed = await parsePcmWav(file)
    expect(parsed.samples).toBe(4)
    expect(parsed.dataOffset).toBe(44)
    const slice = await pcmSlice(file, parsed, 1, 2)
    expect([...new Int16Array(slice)]).toEqual([-2, 3])
    await expect(pcmSlice(file, parsed, 3, 2)).rejects.toThrow('out of bounds')
  })
  it('rejects resampled-looking and truncated inputs', async () => {
    await expect(parsePcmWav(wav([1, 2], 8000))).rejects.toThrow('16 kHz')
    await expect(parsePcmWav(new Blob([new Uint8Array(12)]))).rejects.toThrow('nonempty')
  })
})

describe('summary projections', () => {
  it('extracts per-call transcripts and keeps server latency in ms', () => {
    const calls = callsFromSummary({ phases: [{ calls: [{ run_id: 'r0',
      directory: '/tmp/suite/call_0', call: { language: 'id' }, status: 'COMPLETE',
      summary: { transcript: 'Halo', worker_id: 'w1', measurements: { final_result_ns: 2e9 } } }] }] })
    expect(calls).toEqual([{ label: 'r0', artifactId: 'call_0', language: 'id',
      transcript: 'Halo', status: 'COMPLETE', worker: 'w1', finalMs: 2000 }])
    expect(formatMs(calls[0].finalMs)).toBe('2000 ms')
    expect(formatBytes(1024)).toBe('1.00 KiB')
    expect(sparklinePoints([0, 1]).split(' ')).toHaveLength(2)
  })
})
