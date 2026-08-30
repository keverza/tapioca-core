<script lang="ts">
  import { onMount } from 'svelte'
  import {
    browserName,
    compareRenderPaths,
    diagnoseReport,
    elapsedMilliseconds,
    readGpuRenderer,
    subscribeBridgeCalls,
    summarize,
    summarizeBridgeCalls,
    type BridgeCallSample,
    type DiagnosticMode,
    type Distribution,
    type PerformanceReport,
  } from './performance'

  let {
    mode = $bindable<DiagnosticMode>(),
    nodeCount,
    edgeCount,
    onclose,
  }: { mode: DiagnosticMode; nodeCount: number; edgeCount: number; onclose: () => void } = $props()

  const CAPTURE_MS = 10_000
  const EMPTY: Distribution = { count: 0, medianMs: null, p95Ms: null, worstMs: null }
  const rollingFrames: number[] = []
  const rollingPointers: number[] = []
  const rollingRawPointers: number[] = []
  const rollingLatencies: number[] = []
  let captureFrames: number[] = []
  let capturePointers: number[] = []
  let captureRawPointers: number[] = []
  let captureLatencies: number[] = []
  let captureLongTasks: number[] = []
  let captureBridgeCalls: BridgeCallSample[] = []
  let previousFrame: number | null = null
  let previousPointer: number | null = null
  let previousRawPointer: number | null = null
  let lastPointer: number | null = null
  let captureStarted = 0
  let captureTimer: number | undefined
  let recording = $state(false)
  let liveFrames = $state<Distribution>(EMPTY)
  let livePointers = $state<Distribution>(EMPTY)
  let liveRawPointers = $state<Distribution>(EMPTY)
  let liveLatencies = $state<Distribution>(EMPTY)
  let reports = $state.raw<PerformanceReport[]>([])
  let logs = $state.raw<string[]>(['Ready. Capture each path while continuously moving or dragging for 10 seconds.'])
  let copied = $state(false)
  let rawPointerSupported = false
  let coalescedPointerTotal = 0
  let coalescedPointerMax = 0

  function cappedPush(values: number[], value: number): void {
    values.push(value)
    if (values.length > 600) values.shift()
  }

  function line(message: string): void {
    logs = [`${new Date().toLocaleTimeString()}  ${message}`, ...logs].slice(0, 12)
  }

  function startCapture(): void {
    captureFrames = []
    capturePointers = []
    captureRawPointers = []
    captureLatencies = []
    captureLongTasks = []
    captureBridgeCalls = []
    previousPointer = null
    previousRawPointer = null
    lastPointer = null
    coalescedPointerTotal = 0
    coalescedPointerMax = 0
    captureStarted = performance.now()
    recording = true
    copied = false
    line(`Started ${mode} capture.`)
    captureTimer = window.setTimeout(stopCapture, CAPTURE_MS)
  }

  function stopCapture(): void {
    if (!recording) return
    recording = false
    if (captureTimer !== undefined) window.clearTimeout(captureTimer)
    const durationMs = performance.now() - captureStarted
    const report: PerformanceReport = {
      capturedAt: new Date().toISOString(),
      durationMs,
      mode,
      host: document.documentElement.dataset.tapiocaHost === 'ready' ? 'DG::Browser' : 'standalone browser',
      browser: browserName(navigator.userAgent),
      nodeCount,
      edgeCount,
      devicePixelRatio: window.devicePixelRatio,
      viewport: { width: window.innerWidth, height: window.innerHeight },
      userAgent: navigator.userAgent,
      gpuRenderer: readGpuRenderer(),
      frames: summarize(captureFrames),
      pointerGaps: summarize(capturePointers),
      rawPointerGaps: summarize(captureRawPointers),
      rawPointerSupported,
      coalescedPointerSamples: { total: coalescedPointerTotal, maxPerEvent: coalescedPointerMax },
      inputToFrame: summarize(captureLatencies),
      longTasks: {
        count: captureLongTasks.length,
        totalMs: captureLongTasks.reduce((total, value) => total + value, 0),
        worstMs: Math.max(0, ...captureLongTasks),
      },
      bridgeCalls: summarizeBridgeCalls(captureBridgeCalls),
    }
    reports = [report, ...reports].slice(0, 8)
    line(
      `Stopped ${mode}: frame p95 ${formatMs(report.frames.p95Ms)}, pointer p95 ${formatMs(report.pointerGaps.p95Ms)}, ${report.bridgeCalls.reduce((total, item) => total + item.count, 0)} bridge calls.`,
    )
  }

  async function copyReports(): Promise<void> {
    const text = JSON.stringify({ reports: [...reports].reverse() }, null, 2)
    try {
      await navigator.clipboard.writeText(text)
    } catch {
      const area = document.createElement('textarea')
      area.value = text
      document.body.append(area)
      area.select()
      document.execCommand('copy')
      area.remove()
    }
    copied = true
    line(`Copied ${reports.length} report(s) as JSON.`)
  }

  function formatMs(value: number | null): string {
    return value === null ? '-' : `${value.toFixed(1)} ms`
  }

  const comparison = $derived.by(() => {
    const flow = reports.find((report) => report.mode === 'flow' || report.mode === 'bare-flow')
    const plain = reports.find((report) => report.mode === 'plain-marker')
    return flow !== undefined && plain !== undefined ? compareRenderPaths(flow, plain) : null
  })

  onMount(() => {
    let frameId = 0
    let refreshTimer = 0
    const frame = (time: number) => {
      if (previousFrame !== null) {
        const gap = time - previousFrame
        cappedPush(rollingFrames, gap)
        if (recording) captureFrames.push(gap)
      }
      previousFrame = time
      if (lastPointer !== null) {
        // A CEF rAF timestamp can describe the frame before a pointer handler
        // that ran before this callback. Use one clock sampled at callback time.
        const latency = elapsedMilliseconds(performance.now(), lastPointer)
        cappedPush(rollingLatencies, latency)
        if (recording) captureLatencies.push(latency)
        lastPointer = null
      }
      frameId = requestAnimationFrame(frame)
    }
    frameId = requestAnimationFrame(frame)

    const pointer = (event: PointerEvent) => {
      const now = event.timeStamp
      if (previousPointer !== null) {
        const gap = now - previousPointer
        cappedPush(rollingPointers, gap)
        if (recording) capturePointers.push(gap)
      }
      previousPointer = now
      lastPointer = performance.now()
      if (recording) {
        const coalescedCount = event.getCoalescedEvents?.().length ?? 0
        coalescedPointerTotal += coalescedCount
        coalescedPointerMax = Math.max(coalescedPointerMax, coalescedCount)
      }
    }
    window.addEventListener('pointermove', pointer, { capture: true, passive: true })

    rawPointerSupported = 'onpointerrawupdate' in window
    const rawPointer = (rawEvent: Event) => {
      const event = rawEvent as PointerEvent
      rawPointerSupported = true
      const now = event.timeStamp
      if (previousRawPointer !== null) {
        const gap = now - previousRawPointer
        cappedPush(rollingRawPointers, gap)
        if (recording) captureRawPointers.push(gap)
      }
      previousRawPointer = now
    }
    window.addEventListener('pointerrawupdate', rawPointer, { capture: true, passive: true })

    refreshTimer = window.setInterval(() => {
      liveFrames = summarize(rollingFrames)
      livePointers = summarize(rollingPointers)
      liveRawPointers = summarize(rollingRawPointers)
      liveLatencies = summarize(rollingLatencies)
    }, 500)

    const unsubscribe = subscribeBridgeCalls((sample) => {
      if (recording) captureBridgeCalls.push(sample)
    })
    let observer: PerformanceObserver | undefined
    if (PerformanceObserver.supportedEntryTypes.includes('longtask')) {
      observer = new PerformanceObserver((list) => {
        if (recording) captureLongTasks.push(...list.getEntries().map((entry) => entry.duration))
      })
      observer.observe({ entryTypes: ['longtask'] })
    }

    return () => {
      cancelAnimationFrame(frameId)
      window.clearInterval(refreshTimer)
      if (captureTimer !== undefined) window.clearTimeout(captureTimer)
      window.removeEventListener('pointermove', pointer, { capture: true })
      window.removeEventListener('pointerrawupdate', rawPointer, { capture: true })
      unsubscribe()
      observer?.disconnect()
    }
  })
</script>

<aside class="performance-panel nodrag nowheel" aria-label="Performance diagnostic">
  <header>
    <div><span>Supported diagnostic</span><strong>Interaction timing</strong></div>
    <button class="close" onclick={onclose} aria-label="Close performance diagnostic">Close</button>
  </header>

  <section class="paths">
    <label><span>Render path</span>
      <select bind:value={mode} disabled={recording}>
        <option value="flow">Full SvelteFlow</option>
        <option value="bare-flow">Bare SvelteFlow</option>
        <option value="plain-marker">Plain moving div</option>
        <option value="raw-marker">Raw-event moving div</option>
      </select>
    </label>
    <p>Full includes graph furniture. Bare keeps the same nodes and wires without the background, controls, or minimap. Plain bypasses Svelte and SvelteFlow for pointer movement.</p>
  </section>

  <section class="live">
    <div><span>Frame median</span><strong>{formatMs(liveFrames.medianMs)}</strong></div>
    <div><span>Frame p95</span><strong>{formatMs(liveFrames.p95Ms)}</strong></div>
    <div><span>Pointer p95</span><strong>{formatMs(livePointers.p95Ms)}</strong></div>
    <div><span>Raw pointer p95</span><strong>{formatMs(liveRawPointers.p95Ms)}</strong></div>
    <div><span>Input to frame p95</span><strong>{formatMs(liveLatencies.p95Ms)}</strong></div>
  </section>

  <div class="capture">
    <button class:recording onclick={recording ? stopCapture : startCapture}>{recording ? 'Stop capture' : 'Capture 10 seconds'}</button>
    <span>{recording ? 'Move continuously now' : `${reports.length} saved run${reports.length === 1 ? '' : 's'}`}</span>
  </div>

  {#if reports[0] !== undefined}
    <section class="result">
      <h2>Latest: {reports[0].mode}</h2>
      {#each diagnoseReport(reports[0]) as finding}<p>{finding}</p>{/each}
      {#if comparison !== null}<p class="comparison">{comparison}</p>{/if}
      <dl>
        <div><dt>Frames</dt><dd>{reports[0].frames.count}</dd></div>
        <div><dt>Worst frame</dt><dd>{formatMs(reports[0].frames.worstMs)}</dd></div>
        <div><dt>Long tasks</dt><dd>{reports[0].longTasks.count}</dd></div>
        <div><dt>Bridge calls</dt><dd>{reports[0].bridgeCalls.reduce((total, item) => total + item.count, 0)}</dd></div>
        <div><dt>Raw events</dt><dd>{reports[0].rawPointerGaps.count}</dd></div>
        <div><dt>Coalesced</dt><dd>{reports[0].coalescedPointerSamples.total}</dd></div>
      </dl>
    </section>
  {/if}

  <section class="log">
    <h2>Log</h2>
    {#each logs as entry}<code>{entry}</code>{/each}
  </section>

  <footer>
    <button onclick={copyReports} disabled={reports.length === 0}>{copied ? 'Copied' : 'Copy JSON report'}</button>
    <button onclick={() => { reports = []; logs = ['Reports cleared.']; copied = false }} disabled={reports.length === 0}>Clear</button>
  </footer>
</aside>

<style>
  .performance-panel { position: absolute; z-index: 8; top: 12px; right: 12px; width: min(390px, calc(100% - 24px)); max-height: calc(100% - 24px); overflow: auto; border: 1px solid #485564; border-top: 3px solid #ffb000; border-radius: 4px; background: rgb(17 22 28 / 97%); color: #dce4ec; box-shadow: 0 18px 60px rgb(0 0 0 / 55%); font: 11px/1.4 'Segoe UI', sans-serif; }
  header { position: sticky; z-index: 1; top: 0; display: flex; align-items: center; justify-content: space-between; padding: 11px 13px; border-bottom: 1px solid #303945; background: #202731; }
  header div { display: grid; }
  header span { color: #ffb000; font-size: 8px; font-weight: 700; letter-spacing: .13em; text-transform: uppercase; }
  header strong { font-size: 15px; }
  button, select { height: 28px; border: 1px solid #465361; border-radius: 3px; background: #252e38; color: #dce4ec; font: 10px 'Segoe UI', sans-serif; }
  button { padding: 0 10px; cursor: pointer; }
  button:disabled { cursor: default; opacity: .4; }
  .close { height: 24px; }
  section { padding: 11px 13px; border-bottom: 1px solid #29323c; }
  .paths label { display: grid; grid-template-columns: 90px 1fr; align-items: center; gap: 8px; }
  .paths p { margin: 8px 0 0; color: #8392a2; font-size: 10px; }
  .live { display: grid; grid-template-columns: 1fr 1fr; gap: 7px; }
  .live div { display: grid; padding: 7px 8px; background: #0d1217; }
  .live span { color: #718092; font-size: 9px; text-transform: uppercase; }
  .live strong { color: #ffcb5b; font: 600 13px ui-monospace, monospace; }
  .capture { display: flex; align-items: center; gap: 10px; padding: 11px 13px; border-bottom: 1px solid #29323c; }
  .capture button { border-color: #bb840d; background: #8c6208; }
  .capture button.recording { border-color: #e36d5b; background: #8a3025; }
  .capture span { color: #8d9baa; font: 10px ui-monospace, monospace; }
  h2 { margin: 0 0 7px; color: #8d9baa; font-size: 9px; letter-spacing: .1em; text-transform: uppercase; }
  .result p { margin: 4px 0; color: #bdc8d3; }
  .result .comparison { margin-top: 8px; padding-left: 8px; border-left: 2px solid #ffb000; color: #ffcb5b; }
  dl { display: grid; grid-template-columns: 1fr 1fr; gap: 3px 12px; margin: 9px 0 0; }
  dl div { display: flex; justify-content: space-between; }
  dt { color: #718092; } dd { margin: 0; font-family: ui-monospace, monospace; }
  .log { display: grid; gap: 4px; }
  .log code { color: #8897a7; font: 9px/1.35 ui-monospace, monospace; overflow-wrap: anywhere; }
  footer { display: flex; gap: 7px; padding: 10px 13px; }
  @media (max-width: 520px) { .performance-panel { top: 6px; right: 6px; width: calc(100% - 12px); max-height: calc(100% - 12px); } }
</style>
