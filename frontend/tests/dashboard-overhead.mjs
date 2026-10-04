import assert from 'node:assert/strict'
import { spawn } from 'node:child_process'
import { writeFile } from 'node:fs/promises'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { chromium } from 'playwright-core'

const frontend = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const root = path.resolve(frontend, '..')
const cli = process.env.ASR_CLI || path.join(root, 'build/release-cpu/asr-cli')
const chrome = process.env.ASR_CHROME || '/usr/bin/google-chrome'
const config = 'configs/qwen_native_ui_overhead.yaml'
const callsPerArm = Number(process.env.ASR_M8_CALLS_PER_ARM || 10)
if (!Number.isInteger(callsPerArm) || callsPerArm < 1 || callsPerArm > 100)
  throw new Error('ASR_M8_CALLS_PER_ARM must be an integer from 1 to 100')
const children = []
let browser

function start(command, args, cwd) {
  const child = spawn(command, args, { cwd, stdio: ['ignore', 'pipe', 'pipe'] })
  let output = ''
  child.stdout.on('data', chunk => { output += chunk.toString() })
  child.stderr.on('data', chunk => { output += chunk.toString() })
  children.push(child)
  return { child, get output() { return output } }
}
async function until(fn, description, timeoutMs = 30000) {
  const deadline = Date.now() + timeoutMs
  let last
  while (Date.now() < deadline) {
    try { const value = await fn(); if (value) return value }
    catch (error) { last = error }
    await new Promise(resolve => setTimeout(resolve, 200))
  }
  throw new Error(`Timed out waiting for ${description}${last ? `: ${last}` : ''}`)
}
async function json(base, target, body) {
  const response = await fetch(`${base}${target}`, body === undefined ? undefined : {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
  })
  const result = await response.json()
  if (!response.ok) throw new Error(`${target}: ${response.status} ${JSON.stringify(result)}`)
  return result
}
const load = calls => ({ kind: 'load', mode: 'direct', calls, concurrency: 1,
  warmups: 0, repetitions: 1, languages: ['en'], overrides: ['audio.realtime_pacing=true'] })
async function suite(base, calls) {
  const start = performance.now()
  const { job_id: id } = await json(base, '/v1/suites', load(calls))
  const completed = await until(async () => {
    const job = await json(base, `/v1/jobs/${id}`)
    return job.status === 'RUNNING' ? null : job
  }, `suite ${id}`, 10 * 60_000)
  if (completed.status !== 'COMPLETE' || completed.result?.completed_calls !== calls)
    throw new Error(`Native suite failed: ${JSON.stringify(completed).slice(0, 1200)}`)
  const result = completed.result
  assert.equal(result.is_mock, false)
  const finalMs = result.phases.flatMap(phase => phase.calls)
    .map(call => call.summary?.measurements?.final_result_ns / 1e6)
  assert.equal(finalMs.length, calls)
  assert.ok(finalMs.every(value => Number.isFinite(value) && value > 0))
  return { suite_id: result.suite_id, wall_seconds: (performance.now() - start) / 1000,
    final_ms: finalMs, p50_final_ms: result.metrics.final_result_ms.p50,
    p95_final_ms: result.metrics.final_result_ms.p95,
    sampled_peak_tree_rss_bytes: result.metrics.sampled_peak_tree_rss_bytes,
    max_tree_cpu_core_equivalents: Math.max(...result.phases.map(phase =>
      phase.resources?.max_tree_cpu_core_equivalents ?? 0)) }
}
function percentile(values, fraction) {
  const sorted = [...values].sort((a, b) => a - b)
  const rank = (sorted.length - 1) * fraction
  const low = Math.floor(rank), high = Math.ceil(rank)
  return sorted[low] + (sorted[high] - sorted[low]) * (rank - low)
}
function aggregate(arms) {
  const samples = arms.flatMap(arm => arm.final_ms)
  return { calls: samples.length, mean_final_ms: samples.reduce((a, b) => a + b, 0) / samples.length,
    p50_final_ms: percentile(samples, .5), p95_final_ms: percentile(samples, .95),
    peak_service_tree_rss_bytes: Math.max(...arms.map(arm => arm.sampled_peak_tree_rss_bytes)),
    mean_suite_wall_seconds: arms.reduce((sum, arm) => sum + arm.wall_seconds, 0) / arms.length }
}
function percentDelta(a, b) { return (b - a) / a * 100 }

try {
  const service = start(cli, ['serve', '--config', config, '--port', '0'], root)
  const port = await until(() => {
    const line = service.output.split('\n').find(item => item.startsWith('{') && item.includes('"port"'))
    return line ? JSON.parse(line).port : null
  }, 'native service port')
  const api = `http://127.0.0.1:${port}`
  const plan = await json(api, '/v1/suites/dry-run', load(callsPerArm))
  assert.equal(plan.allowed, true, `Native preflight: ${JSON.stringify(plan.preflight)}`)
  const preview = start(path.join(frontend, 'node_modules/.bin/vite'),
    ['preview', '--host', '127.0.0.1', '--port', '4173', '--strictPort'], frontend)
  await until(async () => (await fetch('http://127.0.0.1:4173')).ok, 'production dashboard')
  console.log('Native warmup: one measured call discarded from A/B comparison')
  await suite(api, 1)
  const arms = []
  for (const mode of ['closed', 'open', 'open', 'closed']) {
    let page
    let context
    let runtimeRequests = 0
    const browserErrors = []
    if (mode === 'open') {
      browser = await chromium.launch({ executablePath: chrome, headless: true,
        args: ['--no-sandbox', '--disable-dev-shm-usage'] })
      context = await browser.newContext()
      page = await context.newPage()
      await page.addInitScript(origin => localStorage.setItem('asr-service-origin', origin), api)
      page.on('pageerror', error => browserErrors.push(error.message))
      page.on('response', response => {
        if (response.url() === `${api}/v1/runtime` && response.ok()) runtimeRequests++
      })
      await page.goto('http://127.0.0.1:4173')
      await page.getByText('SERVICE ONLINE').waitFor()
      await until(() => runtimeRequests >= 2, 'two runtime telemetry polls', 10000)
    }
    const result = await suite(api, callsPerArm)
    arms.push({ mode, ...result, runtime_requests: runtimeRequests, browser_errors: browserErrors })
    console.log(`${mode}: ${result.suite_id} ${result.p50_final_ms.toFixed(1)} ms p50, ` +
      `${result.p95_final_ms.toFixed(1)} ms p95, ${runtimeRequests} runtime polls`)
    if (page) {
      assert.ok(runtimeRequests >= 3, 'Dashboard telemetry did not remain active through suite')
      assert.deepEqual(browserErrors, [])
      await context.close()
      await browser.close()
      browser = undefined
    }
  }
  const closed = aggregate(arms.filter(arm => arm.mode === 'closed'))
  const open = aggregate(arms.filter(arm => arm.mode === 'open'))
  const deltas = {
    p50_final_percent: percentDelta(closed.p50_final_ms, open.p50_final_ms),
    p95_final_percent: percentDelta(closed.p95_final_ms, open.p95_final_ms),
    mean_suite_wall_percent: percentDelta(closed.mean_suite_wall_seconds, open.mean_suite_wall_seconds),
    peak_service_tree_rss_percent: percentDelta(closed.peak_service_tree_rss_bytes,
      open.peak_service_tree_rss_bytes),
  }
  const report = { schema_version: 1, timestamp_utc: new Date().toISOString(),
    engine: 'qwen_native', fixture: 'tests/fixtures/m8_jfk_1p2s.wav', config,
    sequence: 'closed-open-open-closed', calls_per_arm: callsPerArm,
    browser: 'headless system Google Chrome, production dashboard served on loopback',
    backend: 'one persistent local C++ service; direct suites via the same REST API',
    note: 'Type-7 p95 is descriptive below 20 calls per condition; browser memory is outside service-tree RSS.',
    arms, closed, open, deltas,
    screening_pass: closed.calls >= 20 && open.calls >= 20 &&
      deltas.p50_final_percent <= 10 && deltas.p95_final_percent <= 10 &&
      deltas.mean_suite_wall_percent <= 10 && deltas.peak_service_tree_rss_percent <= 5 }
  const target = path.join(root, 'results/m8_dashboard_overhead.json')
  await writeFile(target, JSON.stringify(report, null, 2) + '\n')
  console.log(JSON.stringify({ report: target, closed, open, deltas,
    screening_pass: report.screening_pass }))
  if (!report.screening_pass) process.exitCode = 1
} finally {
  await browser?.close()
  for (const child of children.reverse()) {
    child.kill('SIGTERM')
    await until(() => child.exitCode !== null || child.signalCode !== null, 'child shutdown', 3000)
      .catch(() => child.kill('SIGKILL'))
  }
}
