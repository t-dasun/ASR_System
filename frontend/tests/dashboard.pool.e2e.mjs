import assert from 'node:assert/strict'
import { writeFile } from 'node:fs/promises'
import { chromium } from 'playwright-core'

// Read-only check against an already running C++ pool; starts no ASR calls/jobs.
const api = process.argv[2]
const destination = process.argv[3]
assert.ok(api && new URL(api).hostname === '127.0.0.1', 'Supply the local C++ service URL')
const capabilities = await (await fetch(`${api}/v1/capabilities`)).json()
assert.ok(capabilities.engine.startsWith('qwen_prefix_'))
const browser = await chromium.launch({ executablePath: process.env.ASR_CHROME || '/usr/bin/google-chrome',
  headless: true, args: ['--no-sandbox', '--disable-dev-shm-usage'] })
try {
  const page = await browser.newPage()
  const errors = []
  page.on('pageerror', error => errors.push(error.message))
  page.on('console', message => { if (message.type() === 'error') errors.push(message.text()) })
  await page.addInitScript(origin => localStorage.setItem('asr-service-origin', origin), api)
  await page.goto('http://127.0.0.1:5173')
  await page.getByText('SERVICE ONLINE').waitFor()
  await page.locator('.runtime-panel tbody tr').first().waitFor()
  const editor = page.locator('.experiment-panel')
  assert.equal(await editor.getByLabel('Concurrency', { exact: true }).getAttribute('max'),
    String(Math.min(16, capabilities.worker_processes * capabilities.max_sessions_per_process)))
  assert.ok(await editor.getByLabel('Worker processes').isDisabled())
  assert.equal(await editor.getByLabel('Worker processes').inputValue(), String(capabilities.worker_processes))
  assert.equal(await editor.getByLabel('Calls per repetition').inputValue(), String(capabilities.manifest_inputs))
  assert.equal(await editor.getByLabel('Suite').locator('option[value=sweep]').evaluate(option => option.disabled), true)
  assert.equal(await page.locator('.runtime-panel tbody tr').count(), capabilities.worker_processes)
  for (const heading of ['PID', 'Sessions', 'Queue'])
    assert.ok(await page.locator('.runtime-panel').getByRole('columnheader', { name: heading, exact: true }).isVisible())
  await editor.getByRole('button', { name: 'Dry run', exact: true }).click()
  await page.getByText(`${capabilities.manifest_inputs} planned`).waitFor()
  assert.deepEqual(errors, [])
  const result = { status: 'PASS', service: api, engine: capabilities.engine,
    workers: capabilities.worker_processes, slots_per_worker: capabilities.max_sessions_per_process,
    manifest_inputs: capabilities.manifest_inputs, concurrency_limit: await editor.getByLabel('Concurrency', { exact: true }).getAttribute('max'),
    browser_errors: errors.length, checks: ['manifest_controls', 'worker_layout', 'runtime_pid_sessions_queue', 'cpp_manifest_dry_run'] }
  if (destination) await writeFile(destination, JSON.stringify(result, null, 2) + '\n')
  console.log(JSON.stringify(result))
} finally {
  await browser.close()
}
