import assert from 'node:assert/strict'
import { spawn } from 'node:child_process'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { chromium } from 'playwright-core'

const frontend = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const root = path.resolve(frontend, '..')
const executablePath = process.env.ASR_CHROME || '/usr/bin/google-chrome'
const cli = process.env.ASR_CLI || path.join(root, 'build/release-cpu/asr-cli')
const host = 'http://127.0.0.1:5173'
const children = []
let browser

function start(executable, args, cwd) {
  const child = spawn(executable, args, { cwd, stdio: ['ignore', 'pipe', 'pipe'] })
  let output = ''
  child.stdout.on('data', chunk => { output += chunk.toString() })
  child.stderr.on('data', chunk => { output += chunk.toString() })
  children.push(child)
  return { child, get output() { return output } }
}
async function until(fn, description, timeoutMs = 15000) {
  const deadline = Date.now() + timeoutMs
  let last
  while (Date.now() < deadline) {
    try {
      const value = await fn()
      if (value) return value
    } catch (error) { last = error }
    await new Promise(resolve => setTimeout(resolve, 100))
  }
  throw new Error(`Timed out waiting for ${description}${last ? `: ${last}` : ''}`)
}
function wav(seconds) {
  const samples = Math.round(seconds * 16000)
  const data = Buffer.alloc(44 + samples * 2)
  data.write('RIFF', 0); data.writeUInt32LE(data.length - 8, 4); data.write('WAVE', 8)
  data.write('fmt ', 12); data.writeUInt32LE(16, 16); data.writeUInt16LE(1, 20)
  data.writeUInt16LE(1, 22); data.writeUInt32LE(16000, 24); data.writeUInt32LE(32000, 28)
  data.writeUInt16LE(2, 32); data.writeUInt16LE(16, 34)
  data.write('data', 36); data.writeUInt32LE(samples * 2, 40)
  for (let i = 0; i < samples; ++i) data.writeInt16LE(Math.round(Math.sin(i * .04) * 1000), 44 + i * 2)
  return data
}

try {
  const service = start(cli, ['serve', '--config', 'configs/mock_baseline.yaml', '--port', '0'], root)
  const port = await until(() => {
    const line = service.output.split('\n').find(item => item.startsWith('{') && item.includes('"port"'))
    return line ? JSON.parse(line).port : null
  }, 'C++ service port')
  const api = `http://127.0.0.1:${port}`
  const vite = start(path.join(frontend, 'node_modules/.bin/vite'),
    ['--host', '127.0.0.1', '--port', '5173', '--strictPort'], frontend)
  await until(async () => (await fetch(host)).ok, 'Vite page')
  browser = await chromium.launch({ executablePath, headless: true, args: ['--no-sandbox', '--disable-dev-shm-usage'] })
  const context = await browser.newContext({ acceptDownloads: true })
  const page = await context.newPage()
  const errors = []
  await page.addInitScript(origin => localStorage.setItem('asr-service-origin', origin), api)
  page.on('pageerror', error => errors.push(error.message))
  page.on('console', message => { if (message.type() === 'error') errors.push(message.text()) })
  await page.goto(host)
  await page.getByRole('textbox', { name: 'Service origin' }).fill(api)
  await page.getByRole('button', { name: 'Connect' }).click()
  await page.getByText('SERVICE ONLINE').waitFor()
  await page.getByText('Interactive worker slots').waitFor()
  const file = page.locator('input[type=file]')
  for (const language of ['English', 'Indonesian', 'Mandarin']) {
    await file.setInputFiles({ name: `${language}.wav`, mimeType: 'audio/wav', buffer: wav(.8) })
    await page.getByLabel('Language').selectOption({ label: language })
    await page.getByRole('button', { name: 'Start stream' }).click()
    await page.locator('.streaming-panel .state-pill.completed').waitFor({ timeout: 12000 })
    assert.match(await page.locator('.streaming-panel .progress-label strong').innerText(), /4\s*\/\s*4/)
    assert.ok(Number((await page.locator('.transcript-box .subhead span').innerText()).split(' ')[0]) > 0)
    const transcript = await page.locator('.transcript-box > p').innerText()
    await page.getByLabel('Reference transcript (optional)').fill(transcript)
    await page.getByText('0.00%', { exact: true }).waitFor()
    assert.doesNotMatch(await page.locator('.streaming-panel .stat').filter({ hasText: 'First text arrival' }).innerText(), /—/)
    assert.doesNotMatch(await page.locator('.streaming-panel .stat').filter({ hasText: 'EOF to final text' }).innerText(), /—/)
    await page.getByRole('button', { name: 'Reset' }).click()
  }
  await file.setInputFiles({ name: 'stop.wav', mimeType: 'audio/wav', buffer: wav(3) })
  await page.getByRole('button', { name: 'Start stream' }).click()
  await page.locator('.streaming-panel .state-pill.streaming').waitFor({ timeout: 10000 })
  await page.getByRole('button', { name: 'Stop', exact: true }).click()
  await page.locator('.streaming-panel .state-pill.stopped').waitFor({ timeout: 10000 })
  await page.getByRole('button', { name: 'Reset' }).click()

  await page.getByRole('button', { name: 'Resolve config' }).click()
  await page.getByText('Resolved configuration', { exact: true }).waitFor()
  await page.getByRole('button', { name: 'Dry run' }).click()
  await page.getByText('2 planned').waitFor()
  await page.getByLabel('Suite', { exact: true }).selectOption('sweep')
  await page.getByLabel('Strategy').selectOption('oat')
  await page.getByRole('button', { name: 'Dry run' }).click()
  await page.getByText('4 planned').waitFor()
  await page.getByLabel('Suite', { exact: true }).selectOption('load')
  await page.getByLabel('Calls per repetition').fill('1')
  for (let index = 0; index < 2; ++index) {
    await page.getByRole('button', { name: 'Start suite' }).click()
    await page.locator('.job-card .state-pill.complete').waitFor({ timeout: 20000 })
    await until(async () => (await page.locator('.history-panel select').first().locator('option').count()) >= index + 2,
      'history refresh')
  }
  const options = await page.locator('.history-panel select').first().locator('option').evaluateAll(nodes =>
    nodes.map(node => node.value).filter(Boolean))
  assert.ok(options.length >= 2)
  await page.getByLabel('Run A').selectOption(options.at(-1))
  await page.getByLabel('Run B').selectOption(options.at(-2))
  await page.locator('.run-card .call-row').first().waitFor()
  const download = page.waitForEvent('download')
  await page.locator('.run-card').first().getByRole('button', { name: 'summary.json' }).click()
  assert.match((await download).suggestedFilename(), /summary\.json$/)
  await page.getByLabel('Level').selectOption('error')
  await page.getByText('No matching dashboard events.').waitFor()
  await page.getByLabel('Level').selectOption('all')
  assert.ok(await page.locator('.log-entry').count() > 0)
  assert.deepEqual(errors, [], `Browser console errors: ${errors.join('; ')}`)
  console.log(JSON.stringify({ status: 'PASS', languages: ['en', 'id', 'zh'],
    stop_reset: true, resolve_dry_run: true, sweep_dry_run: true, jobs: 2,
    comparison: true, download: true, log_filter: true,
    browser_errors: errors.length, service_port: port }))
} finally {
  await browser?.close()
  for (const child of children.reverse()) {
    child.kill('SIGTERM')
    await until(() => child.exitCode !== null || child.signalCode !== null, 'child shutdown', 3000).catch(() => child.kill('SIGKILL'))
  }
}
