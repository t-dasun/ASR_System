export interface PcmWav {
  dataOffset: number
  samples: number
  durationSeconds: number
  sampleRate: 16000
  channels: 1
  bitsPerSample: 16
}

const ascii = (bytes: Uint8Array): string => String.fromCharCode(...bytes)

async function sliceBytes(file: Blob, start: number, length: number): Promise<Uint8Array> {
  const bytes = new Uint8Array(await file.slice(start, start + length).arrayBuffer())
  if (bytes.length !== length) throw new Error('WAV is truncated')
  return bytes
}

/** Parse only the header. PCM data is read in bounded slices during streaming. */
export async function parsePcmWav(file: Blob): Promise<PcmWav> {
  if (file.size < 44 || file.size > 60 * 1024 * 1024) {
    throw new Error('Select a nonempty WAV smaller than 60 MiB')
  }
  const riff = await sliceBytes(file, 0, 12)
  if (ascii(riff.slice(0, 4)) !== 'RIFF' || ascii(riff.slice(8, 12)) !== 'WAVE') {
    throw new Error('Only classic RIFF/WAVE files are supported')
  }
  let offset = 12
  let formatOk = false
  let dataOffset = -1
  let dataBytes = 0
  let chunks = 0
  while (offset + 8 <= file.size && chunks++ < 64) {
    const header = await sliceBytes(file, offset, 8)
    const view = new DataView(header.buffer)
    const id = ascii(header.slice(0, 4))
    const size = view.getUint32(4, true)
    const content = offset + 8
    if (content + size > file.size) throw new Error('WAV chunk exceeds file size')
    if (id === 'fmt ') {
      if (size < 16 || size > 256) throw new Error('Unsupported WAV format chunk')
      const fmt = new DataView((await sliceBytes(file, content, 16)).buffer)
      const encoding = fmt.getUint16(0, true)
      const channels = fmt.getUint16(2, true)
      const rate = fmt.getUint32(4, true)
      const byteRate = fmt.getUint32(8, true)
      const blockAlign = fmt.getUint16(12, true)
      const bits = fmt.getUint16(14, true)
      formatOk = encoding === 1 && channels === 1 && rate === 16000 &&
        byteRate === 32000 && blockAlign === 2 && bits === 16
    } else if (id === 'data') {
      if (dataOffset >= 0) throw new Error('Multiple WAV data chunks are unsupported')
      dataOffset = content
      dataBytes = size
    }
    offset = content + size + (size & 1)
  }
  if (!formatOk) throw new Error('Use uncompressed 16 kHz, mono, signed 16-bit PCM WAV')
  if (dataOffset < 0 || dataBytes === 0 || dataBytes % 2 !== 0) {
    throw new Error('WAV PCM data is missing or misaligned')
  }
  const samples = dataBytes / 2
  if (samples > 16000 * 3600) throw new Error('WAV exceeds one hour')
  return { dataOffset, samples, durationSeconds: samples / 16000, sampleRate: 16000,
    channels: 1, bitsPerSample: 16 }
}

export async function pcmSlice(file: Blob, wav: PcmWav, firstSample: number,
                               sampleCount: number): Promise<ArrayBuffer> {
  if (!Number.isInteger(firstSample) || !Number.isInteger(sampleCount) || firstSample < 0 ||
      sampleCount <= 0 || firstSample + sampleCount > wav.samples || sampleCount > 16000) {
    throw new Error('PCM slice is out of bounds')
  }
  const begin = wav.dataOffset + firstSample * 2
  const bytes = await file.slice(begin, begin + sampleCount * 2).arrayBuffer()
  if (bytes.byteLength !== sampleCount * 2) throw new Error('PCM slice is truncated')
  return bytes
}
