import { DEFAULT_DISPLAY_STATE, type DisplayState } from './types/display.ts'
import type { NodeViewMode, NodeVisualState } from './types/node'
import type { PortLayout } from './types/port'

export const NODE_PRESENTATION_DOCUMENT_VERSION = 1

export interface SerializedNodePresentation {
  nodeId: string
  nickname?: string
  color?: string
  viewMode: NodeViewMode
  portLayout: PortLayout
  display: DisplayState
  size?: { width: number; height: number }
}

export interface NodePresentationDocument {
  version: typeof NODE_PRESENTATION_DOCUMENT_VERSION
  nodes: SerializedNodePresentation[]
}

function record(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
}

function viewMode(value: unknown): NodeViewMode {
  return value === 'compact' || value === 'expanded' ? value : 'standard'
}

function portLayout(value: unknown): PortLayout {
  return value === 'vertical' ? 'vertical' : 'horizontal'
}

export function serializeNodePresentations(visuals: Map<string, NodeVisualState>): NodePresentationDocument {
  return {
    version: NODE_PRESENTATION_DOCUMENT_VERSION,
    nodes: [...visuals.entries()].map(([nodeId, visual]) => ({
      nodeId,
      nickname: visual.nickname,
      color: visual.color,
      viewMode: visual.viewMode ?? 'standard',
      portLayout: visual.portLayout ?? 'horizontal',
      display: visual.display ?? DEFAULT_DISPLAY_STATE,
      size: visual.size,
    })),
  }
}

export function parseNodePresentations(value: unknown): Map<string, NodeVisualState> {
  if (!record(value)) throw new Error('Node presentation document must be an object.')
  const version = value.version === undefined ? 0 : value.version
  if (version !== 0 && version !== NODE_PRESENTATION_DOCUMENT_VERSION) throw new Error(`Unsupported node presentation version: ${String(version)}`)
  const nodes = Array.isArray(value.nodes)
    ? value.nodes
    : version === 0
      ? Object.entries(value).map(([nodeId, visual]) => ({ nodeId, ...(record(visual) ? visual : {}) }))
      : undefined
  if (nodes === undefined) throw new Error('Node presentation document requires a nodes array.')

  const result = new Map<string, NodeVisualState>()
  for (const item of nodes) {
    if (!record(item) || typeof item.nodeId !== 'string' || item.nodeId === '') throw new Error('Each node presentation requires a nodeId.')
    const nickname = typeof item.nickname === 'string' ? item.nickname : typeof item.name === 'string' ? item.name : undefined
    const color = typeof item.color === 'string' ? item.color : undefined
    result.set(item.nodeId, {
      nickname,
      color,
      viewMode: viewMode(item.viewMode),
      portLayout: portLayout(item.portLayout),
      display: record(item.display) ? { ...DEFAULT_DISPLAY_STATE, ...item.display, style: { ...DEFAULT_DISPLAY_STATE.style, ...(record(item.display.style) ? item.display.style : {}) } } as DisplayState : DEFAULT_DISPLAY_STATE,
      size: record(item.size) && typeof item.size.width === 'number' && typeof item.size.height === 'number' ? { width: item.size.width, height: item.size.height } : undefined,
    })
  }
  return result
}
