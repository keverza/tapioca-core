export type DiagnosticMode = 'flow' | 'bare-flow' | 'plain-marker' | 'raw-marker'

export interface Distribution {
  count: number
  medianMs: number | null
  p95Ms: number | null
  worstMs: number | null
}

export interface BridgeCallSample {
  command: string
  durationMs: number
  succeeded: boolean
}

export interface BridgeCallSummary {
  command: string
  count: number
  meanMs: number
  p95Ms: number
  worstMs: number
  failures: number
}

export interface PerformanceReport {
  capturedAt: string
  durationMs: number
  mode: DiagnosticMode
  host: 'DG::Browser' | 'WebView2 child HWND' | 'standalone browser'
  browser: string
  nodeCount: number
  edgeCount: number
  devicePixelRatio: number
  viewport: { width: number; height: number }
  userAgent: string
  gpuRenderer: string
  frames: Distribution
  pointerGaps: Distribution
  rawPointerGaps: Distribution
  rawPointerSupported: boolean
  coalescedPointerSamples: { total: number; maxPerEvent: number }
  inputToFrame: Distribution
  longTasks: { count: number; totalMs: number; worstMs: number }
  bridgeCalls: BridgeCallSummary[]
  nativeTelemetry: {
    actualDisplayRefreshHz: number | null
    topology: string
    archicadSwapChainIntercepted: boolean
  }
  lowLevelTimings: {
    renderCpuMs: null
    gpuFrameMs: null
    presentMs: null
    waitForFrameMs: null
    source: string
  }
}

declare global {
  interface Window {
    __tapiocaHost?: string
    __tapiocaNativeTelemetry?: {
      actualDisplayRefreshHz?: number
      topology?: string
      archicadSwapChainIntercepted?: boolean
    }
  }
}

export function hostName(marker: string | undefined): PerformanceReport['host'] {
  if (marker === 'ready') return 'DG::Browser'
  if (marker === 'webview2-child') return 'WebView2 child HWND'
  return 'standalone browser'
}

type BridgeListener = (sample: BridgeCallSample) => void
const bridgeListeners = new Set<BridgeListener>()

export function publishBridgeCall(sample: BridgeCallSample): void {
  for (const listener of bridgeListeners) listener(sample)
}

export function subscribeBridgeCalls(listener: BridgeListener): () => void {
  bridgeListeners.add(listener)
  return () => bridgeListeners.delete(listener)
}

function percentile(sorted: number[], ratio: number): number | null {
  if (sorted.length === 0) return null
  return sorted[Math.floor((sorted.length - 1) * ratio)]
}

export function summarize(values: number[]): Distribution {
  const sorted = [...values.filter(Number.isFinite)].sort((left, right) => left - right)
  return {
    count: sorted.length,
    medianMs: percentile(sorted, 0.5),
    p95Ms: percentile(sorted, 0.95),
    worstMs: sorted.at(-1) ?? null,
  }
}

export function elapsedMilliseconds(now: number, before: number): number {
  return Math.max(0, now - before)
}

export function summarizeBridgeCalls(samples: BridgeCallSample[]): BridgeCallSummary[] {
  const grouped = new Map<string, BridgeCallSample[]>()
  for (const sample of samples) grouped.set(sample.command, [...(grouped.get(sample.command) ?? []), sample])

  return [...grouped.entries()]
    .map(([command, calls]) => {
      const durations = calls.map((call) => call.durationMs)
      const distribution = summarize(durations)
      return {
        command,
        count: calls.length,
        meanMs: durations.reduce((total, value) => total + value, 0) / calls.length,
        p95Ms: distribution.p95Ms ?? 0,
        worstMs: distribution.worstMs ?? 0,
        failures: calls.filter((call) => !call.succeeded).length,
      }
    })
    .sort((left, right) => left.command.localeCompare(right.command))
}

export function diagnoseReport(report: PerformanceReport): string[] {
  const findings: string[] = []
  if (report.longTasks.count > 0) findings.push('JavaScript/main-thread stalls occurred during the capture.')
  if ((report.pointerGaps.p95Ms ?? 0) >= 40) findings.push('Pointer events arrived in noticeable bursts (p95 >= 40 ms).')
  if (report.rawPointerGaps.count > report.pointerGaps.count * 1.5) {
    findings.push('Raw pointer updates arrived materially more often than pointermove events; a raw-input drag path may help.')
  } else if (report.rawPointerSupported && report.rawPointerGaps.count > 0) {
    findings.push('Raw pointer updates did not provide a materially higher event cadence.')
  } else {
    findings.push('Raw pointer updates were unavailable or produced no events.')
  }
  if ((report.frames.medianMs ?? 0) >= 28) {
    findings.push('Frame cadence was approximately 30 Hz or slower.')
  } else if ((report.frames.p95Ms ?? 0) >= 32) {
    findings.push('Frame cadence was usually fast but had visible missed-frame spikes.')
  }
  if ((report.inputToFrame.p95Ms ?? 0) >= 40) findings.push('Pointer-to-next-frame latency was clearly noticeable.')
  if (report.bridgeCalls.length === 0) findings.push('No native bridge calls occurred during the capture.')
  return findings.length > 0 ? findings : ['No obvious cadence, input, long-task, or bridge signature was detected.']
}

export function compareRenderPaths(flow: PerformanceReport, plain: PerformanceReport): string {
  const flowP95 = flow.frames.p95Ms
  const plainP95 = plain.frames.p95Ms
  if (flowP95 === null || plainP95 === null) return 'Not enough frame samples to compare render paths.'
  if (flowP95 >= plainP95 * 1.35 && flowP95 - plainP95 >= 5) {
    return 'The flow path is materially slower than the plain marker path; focus on SvelteFlow, custom nodes, or CSS.'
  }
  if (flowP95 >= 28 && plainP95 >= 28 && Math.abs(flowP95 - plainP95) <= 6) {
    return 'Both render paths are similarly cadence-limited; the shared browser/host presentation path is implicated.'
  }
  return 'The two render paths do not yet separate the bottleneck clearly.'
}

export function readGpuRenderer(): string {
  const canvas = document.createElement('canvas')
  const gl = canvas.getContext('webgl')
  if (gl === null) return 'WebGL unavailable'
  const extension = gl.getExtension('WEBGL_debug_renderer_info')
  if (extension !== null) return String(gl.getParameter(extension.UNMASKED_RENDERER_WEBGL))
  return String(gl.getParameter(gl.RENDERER))
}

export function browserName(userAgent: string): string {
  if (userAgent.includes('Firefox/')) return 'Firefox'
  if (userAgent.includes('Edg/')) return 'Edge'
  if (userAgent.includes('Chrome/')) return 'Chrome/Chromium'
  return 'Unknown browser'
}
