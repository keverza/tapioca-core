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
