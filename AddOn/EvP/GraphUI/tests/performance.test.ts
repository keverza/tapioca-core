import assert from 'node:assert/strict'
import test from 'node:test'
import {
  browserName,
  compareRenderPaths,
  diagnoseReport,
  elapsedMilliseconds,
  summarize,
  summarizeBridgeCalls,
  type PerformanceReport,
} from '../src/performance.ts'

function report(mode: PerformanceReport['mode'], frameP95: number): PerformanceReport {
  return {
    capturedAt: '2026-08-30T00:00:00.000Z',
    durationMs: 10_000,
    mode,
    host: 'DG::Browser',
    browser: 'Chrome/Chromium',
    nodeCount: 2,
    edgeCount: 1,
    devicePixelRatio: 1,
    viewport: { width: 800, height: 600 },
    userAgent: 'test',
    gpuRenderer: 'test',
    frames: { count: 300, medianMs: 16.7, p95Ms: frameP95, worstMs: frameP95 },
    pointerGaps: { count: 300, medianMs: 8, p95Ms: 12, worstMs: 16 },
    rawPointerGaps: { count: 300, medianMs: 8, p95Ms: 12, worstMs: 16 },
    rawPointerSupported: true,
    coalescedPointerSamples: { total: 300, maxPerEvent: 1 },
    inputToFrame: { count: 300, medianMs: 5, p95Ms: 12, worstMs: 16 },
    longTasks: { count: 0, totalMs: 0, worstMs: 0 },
    bridgeCalls: [],
    nativeTelemetry: {
      actualDisplayRefreshHz: null,
      topology: 'browser control',
      archicadSwapChainIntercepted: false,
    },
    lowLevelTimings: {
      renderCpuMs: null,
      gpuFrameMs: null,
      presentMs: null,
      waitForFrameMs: null,
      source: 'not exposed',
    },
  }
}

test('summarize reports deterministic nearest-rank percentiles', () => {
  assert.deepEqual(summarize([50, 10, 30, 20, 40]), {
    count: 5,
    medianMs: 30,
    p95Ms: 40,
    worstMs: 50,
  })
  assert.deepEqual(summarize([]), { count: 0, medianMs: null, p95Ms: null, worstMs: null })
})

test('elapsed time never reports impossible negative latency', () => {
  assert.equal(elapsedMilliseconds(101.5, 100), 1.5)
  assert.equal(elapsedMilliseconds(99.5, 100), 0)
})

test('browser name does not mislabel Firefox as Chromium', () => {
  assert.equal(browserName('Mozilla/5.0 Firefox/153.0'), 'Firefox')
  assert.equal(browserName('Mozilla/5.0 Chrome/128.0.0.0 Edg/128.0.0.0'), 'Edge')
})

test('bridge summaries retain command counts, latency, and failures', () => {
  assert.deepEqual(
    summarizeBridgeCalls([
      { command: 'Tapioca.GraphGetState', durationMs: 4, succeeded: true },
      { command: 'Tapioca.GraphGetState', durationMs: 8, succeeded: false },
      { command: 'Tapioca.GraphApplyEdit', durationMs: 2, succeeded: true },
    ]),
    [
      { command: 'Tapioca.GraphApplyEdit', count: 1, meanMs: 2, p95Ms: 2, worstMs: 2, failures: 0 },
      { command: 'Tapioca.GraphGetState', count: 2, meanMs: 6, p95Ms: 4, worstMs: 8, failures: 1 },
    ],
  )
})

test('diagnosis reports cadence and bridge facts without assigning an unproved cause', () => {
  const slow = report('flow', 52)
  slow.frames.medianMs = 33.3
  slow.pointerGaps.p95Ms = 60
  slow.inputToFrame.p95Ms = 45
  slow.longTasks = { count: 1, totalMs: 70, worstMs: 70 }

  assert.deepEqual(diagnoseReport(slow), [
    'JavaScript/main-thread stalls occurred during the capture.',
    'Pointer events arrived in noticeable bursts (p95 >= 40 ms).',
    'Raw pointer updates did not provide a materially higher event cadence.',
    'Frame cadence was approximately 30 Hz or slower.',
    'Pointer-to-next-frame latency was clearly noticeable.',
    'No native bridge calls occurred during the capture.',
  ])
})

test('diagnosis identifies materially higher raw pointer cadence', () => {
  const raw = report('raw-marker', 16)
  raw.pointerGaps.count = 100
  raw.rawPointerGaps = { count: 300, medianMs: 4, p95Ms: 8, worstMs: 12 }
  assert.match(diagnoseReport(raw).join(' '), /Raw pointer updates arrived materially more often/)
})

test('render-path comparison separates flow work from a shared host cadence limit', () => {
  assert.match(compareRenderPaths(report('flow', 38), report('plain-marker', 18)), /flow path is materially slower/)
  assert.match(compareRenderPaths(report('flow', 34), report('plain-marker', 32)), /shared browser\/host/)
})
