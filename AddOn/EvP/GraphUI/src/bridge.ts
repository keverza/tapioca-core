import { publishBridgeCall } from './performance'

type EvPCallReturn = string | PromiseLike<string>

interface EvPBridge {
  call(requestJson: string): EvPCallReturn
}

interface EvPFailure {
  ok: false
  error: { message: string; code?: string }
}

interface EvPSuccess<T> {
  ok: true
  data: T
}

declare global {
  interface Window {
    EvP?: EvPBridge
  }
}

export async function callTapioca<T>(command: string, params: Record<string, unknown> = {}): Promise<T> {
  if (window.EvP === undefined || typeof window.EvP.call !== 'function') {
    throw new Error('The native EvP.call bridge is not available.')
  }

  const started = performance.now()
  let succeeded = false
  try {
    const raw = await Promise.resolve(window.EvP.call(JSON.stringify({ command, params })))
    const envelope = JSON.parse(String(raw)) as EvPSuccess<T> | EvPFailure
    if (!envelope.ok) {
      throw new Error(envelope.error.message)
    }
    succeeded = true
    return envelope.data
  } finally {
    publishBridgeCall({ command, durationMs: performance.now() - started, succeeded })
  }
}

export function isNativeBridgeAvailable(): boolean {
  return window.EvP !== undefined && typeof window.EvP.call === 'function'
}

/**
 * Resolves once the native bridge exists, or false once it plainly will not.
 *
 * ⚠️ SAMPLING THE BRIDGE ONCE AT STARTUP IS A RACE IN ONE OF THE TWO HOSTS.
 * The WebView2 palette installs `window.EvP` with
 * AddScriptToExecuteOnDocumentCreated, so it is there before this module's
 * first line and the first check already passes. DG::Browser registers it with
 * RegisterAsynchJSObject, and the clue is in the name: it can arrive after the
 * bundle has booted. Losing that race is not a visible error - the editor falls
 * back to its two-node fixture and the component picker looks like the whole
 * catalog is two nodes.
 *
 * So wait a moment before believing there is no runtime. The cost when there
 * genuinely is none - a standalone browser - is one short delay at startup, and
 * the fixture it then shows is a diagnostic, not something anyone is waiting on.
 */
export async function waitForNativeBridge(timeoutMs = 3000): Promise<boolean> {
  if (isNativeBridgeAvailable()) return true
  const deadline = performance.now() + timeoutMs
  while (performance.now() < deadline) {
    await new Promise((resolve) => setTimeout(resolve, 50))
    if (isNativeBridgeAvailable()) return true
  }
  return false
}
