import assert from 'node:assert/strict'
import { spawn } from 'node:child_process'
import { mkdir, readFile, writeFile } from 'node:fs/promises'
import { createHash } from 'node:crypto'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { chromium } from 'playwright-core'

const frontend = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const root = path.resolve(frontend, '..')
const cli = process.env.ASR_CLI || path.join(root, 'build/release-cpu/asr-cli')
const chrome = process.env.ASR_CHROME || '/usr/bin/google-chrome'
const children = []
let browser
const evidenceDirectory = process.env.ASR_DEMO_EVIDENCE_DIR
const sha256 = async file => createHash('sha256').update(await readFile(file)).digest('hex')

function start(command, args, cwd) {
  const child = spawn(command, args, { cwd, stdio: ['ignore', 'pipe', 'pipe'] })
  let output = ''
  child.stdout.on('data', chunk => { output += chunk.toString() })
  child.stderr.on('data', chunk => { output += chunk.toString() })
  children.push(child)
  return { child, get output() { return output } }
}
async function until(fn, label, timeoutMs = 15000) {
  const deadline = Date.now() + timeoutMs
  let last
  while (Date.now() < deadline) {
    try { const value = await fn(); if (value) return value }
    catch (error) { last = error }
    await new Promise(resolve => setTimeout(resolve, 100))
  }
  throw new Error(`Timed out waiting for ${label}${last ? `: ${last}` : ''}`)
}

try {
  if (evidenceDirectory) await mkdir(evidenceDirectory)
  const manifest = (await readFile(path.join(root, 'datasets/manifests/fleurs_tuning.jsonl'), 'utf8'))
    .trim().split('\n').map(line => JSON.parse(line))
  const clips = ['en', 'id', 'zh'].map(language => {
    const match = manifest.filter(row => row.language === language)
      .filter(row => !evidenceDirectory || row.duration_s >= 15)
      .sort((a, b) => a.duration_s - b.duration_s)[0]
    assert.ok(match, `No FLEURS tuning clip for ${language}`)
    return match
  })
  const service = start(cli, ['serve', '--config', 'configs/qwen_native_single.yaml', '--port', '0'], root)
  const port = await until(() => {
    const line = service.output.split('\n').find(item => item.startsWith('{') && item.includes('"port"'))
    return line ? JSON.parse(line).port : null
  }, 'native service port')
  const api = `http://127.0.0.1:${port}`
  start(path.join(frontend, 'node_modules/.bin/vite'),
    ['preview', '--host', '127.0.0.1', '--port', '4173', '--strictPort'], frontend)
  await until(async () => (await fetch('http://127.0.0.1:4173')).ok, 'dashboard preview')
  browser = await chromium.launch({ executablePath: chrome, headless: true,
    args: ['--no-sandbox', '--disable-dev-shm-usage'] })
  const page = await browser.newPage()
  const errors = []
  await page.addInitScript(origin => localStorage.setItem('asr-service-origin', origin), api)
  page.on('pageerror', error => errors.push(error.message))
  page.on('console', message => { if (message.type() === 'error') errors.push(message.text()) })
  await page.goto('http://127.0.0.1:4173')
  await page.getByText('SERVICE ONLINE').waitFor()
  const results = []
  const labels = { en: 'English', id: 'Indonesian', zh: 'Mandarin' }
  for (const clip of clips) {
    await page.locator('input[type=file]').setInputFiles(path.join(root, clip.file))
    await page.getByLabel('Language').selectOption({ label: labels[clip.language] })
    await page.getByRole('button', { name: 'Start stream' }).click()
    await page.locator('.streaming-panel .state-pill.completed').waitFor({ timeout: 90000 })
    const transcript = (await page.locator('.transcript-box p').innerText()).trim()
    assert.ok(transcript && !transcript.includes('Waiting for'),
      `${clip.language} produced no native transcript`)
    const revisions = Number((await page.locator('.transcript-box .subhead span').innerText()).split(' ')[0])
    assert.ok(revisions > 0)
    const stats = await page.locator('.streaming-panel .stat').evaluateAll(nodes =>
      Object.fromEntries(nodes.map(node => [node.querySelector('span')?.textContent,
        node.querySelector('strong')?.textContent])))
    const acked = await page.locator('.streaming-panel .progress-label strong').innerText()
    const firstTextMs = Number(stats['Client first text']?.replace(' ms', ''))
    assert.ok(Number.isFinite(firstTextMs) && firstTextMs > 0,
      `${clip.language} has no first-text timing`)
    const firstTextBeforeEof = firstTextMs < clip.duration_s * 1000
    if (evidenceDirectory)
      assert.ok(firstTextBeforeEof, `${clip.language} first text did not precede audio EOF`)
    results.push({ language: clip.language, clip_id: clip.id, duration_seconds: clip.duration_s,
      transcript, revisions, client_first_text_ms: firstTextMs, first_text_before_eof: firstTextBeforeEof,
      max_client_send_lag: stats['Max client send lag'], acked_chunks: acked,
      source_wav_sha256: clip.sha256 })
    if (evidenceDirectory)
      await page.screenshot({ path: path.join(evidenceDirectory, `${clip.language}-stream-complete.png`) })
    console.log(`${clip.language}: ${clip.id}, ${revisions} revisions, ${transcript}`)
    await page.getByRole('button', { name: 'Reset' }).click()
  }
  assert.deepEqual(errors, [])
  const evidence = { schema_version: 1, status: 'PASS', engine: 'qwen_native',
    mode: 'native C++ service and headless browser over loopback',
    results, browser_errors: errors.length,
    command: 'npm run test:e2e:native',
    config_sha256: await sha256(path.join(root, 'configs/qwen_native_single.yaml')),
    cli_sha256: await sha256(cli),
    worker_sha256: await sha256(path.join(path.dirname(cli), 'asr-native-worker')),
    manifest_sha256: await sha256(path.join(root, 'datasets/manifests/fleurs_tuning.jsonl')),
    model_acquisition: JSON.parse(await readFile(path.join(root, 'models/qwen3-asr-0.6b/acquisition.json'), 'utf8')) }
  if (evidenceDirectory)
    await writeFile(path.join(evidenceDirectory, 'demo.json'), JSON.stringify(evidence, null, 2) + '\n', { flag: 'wx' })
  console.log(JSON.stringify(evidence))
} finally {
  await browser?.close()
  for (const child of children.reverse()) {
    child.kill('SIGTERM')
    await until(() => child.exitCode !== null || child.signalCode !== null, 'child shutdown', 3000)
      .catch(() => child.kill('SIGKILL'))
  }
}
