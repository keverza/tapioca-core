import type { XYPosition } from '@xyflow/svelte'
import type { GraphParameter, NodeResultRecord, NodeTypeSchema } from '../../types'
import type { DisplayState } from './display'
import type { PortDefinition, PortLayout, PortTransform } from './port'
import type { ExecutionMode } from './runtime'

export type NodeBodyMode = 'none' | 'parameters' | 'viewer' | 'parameters+viewer' | 'custom'
export type NodeViewMode = 'compact' | 'standard' | 'expanded'

export interface NodeSize { width: number; height: number }

export interface NodeCapabilities {
  bypass: boolean
  disable: boolean
  freeze: boolean
  resizable: boolean
  nodeViewer: boolean
  canvasPreview: boolean
  overlayPreview: boolean
  floatingViewer: boolean
  floatingControls: boolean
  gumball: boolean
}

export interface NodePresentation {
  bodyMode: NodeBodyMode
  portLayout: PortLayout
  resizable: boolean
  minSize?: NodeSize
  maxSize?: NodeSize
  controls: { inline: boolean; expandable: boolean; detachable: boolean }
  viewer: { enabled: boolean; detachable: boolean; navigation: boolean; gumball: boolean }
}

export interface NodeDefinition extends Omit<NodeTypeSchema, 'inputs' | 'outputs'> {
  version: number
  inputs: PortDefinition[]
  outputs: PortDefinition[]
  presentation: NodePresentation
  capabilities: NodeCapabilities
}

export interface InputState {
  portId: string
  nickname?: string
  connected: boolean
  connectionCount: number
  internalized: boolean
  transforms: PortTransform[]
}

export interface OutputState {
  portId: string
  nickname?: string
  connected: boolean
  connectionCount: number
  selectedForDisplay: boolean
}

export interface NodeInstance {
  id: string
  type: string
  version: number
  nickname?: string
  position: XYPosition
  size?: NodeSize
  viewMode: NodeViewMode
  executionMode: ExecutionMode
  inputs: InputState[]
  outputs: OutputState[]
  parameters: GraphParameter[]
  display: DisplayState
  runtime?: NodeResultRecord
}

export interface NodeVisualState {
  nickname?: string
  color?: string
  viewMode?: NodeViewMode
  portLayout?: PortLayout
  size?: NodeSize
  display?: DisplayState
}
