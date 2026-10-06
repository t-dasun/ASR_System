import { describe, expect, it } from 'vitest'
import { normalizeTranscript, scoreTranscript, summarizeAccuracy } from './accuracy'

describe('completed-call accuracy', () => {
  it('uses words for English/Indonesian and characters for Mandarin', () => {
    expect(scoreTranscript('One two three four.', 'one two four', 'en')?.rate).toBe(.25)
    expect(scoreTranscript('Selamat pagi!', 'selamat', 'id')?.rate).toBe(.5)
    expect(scoreTranscript('你好，世界。', '你好世', 'zh')?.rate).toBe(.25)
    expect(normalizeTranscript('Straße ﬁnal', 'en')).toBe('strasse final')
    expect(scoreTranscript('', 'text', 'en')).toBeNull()
  })
  it('weights reference units and excludes failed/unlabelled calls', () => {
    const rows = [
      { language: 'en', status: 'COMPLETE', reference: 'one two three four', transcript: 'one two three' },
      { language: 'en', status: 'COMPLETE', reference: 'hello', transcript: '' },
      { language: 'en', status: 'FAILED', reference: 'ignored', transcript: '' },
      { language: 'en', status: 'COMPLETE', transcript: 'unlabelled' },
    ]
    expect(summarizeAccuracy(rows)).toEqual([{ language: 'en', metric: 'WER', scored: 2,
      completed: 3, offered: 4, rate: .4 }])
  })
})
