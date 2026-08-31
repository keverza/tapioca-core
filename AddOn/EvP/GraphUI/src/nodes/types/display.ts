import type { NodeTypeSchema } from '../../types'
import type { NodeBodyMode } from './node'

export type DisplayRepresentation = 'default' | 'shaded' | 'wireframe' | 'ghosted' | 'points' | 'bounds'
export type DisplayRole = 'primary' | 'reference' | 'none'

export interface DisplayStyle {
  representation: DisplayRepresentation
  opacity?: number
  lineWidth?: number
  pointSize?: number
  showEdges?: boolean
  showPoints?: boolean
  showLabels?: boolean
  colorMode?: string
}

export interface DisplayState {
  output?: string
  nodeViewer: boolean
  canvasBackground: boolean
  archicadOverlay: boolean
  displayRole: DisplayRole
  style: DisplayStyle
}

export const DEFAULT_DISPLAY_STATE: DisplayState = {
  nodeViewer: true,
  canvasBackground: false,
  archicadOverlay: false,
  displayRole: 'none',
  style: { representation: 'default' },
}

const CATEGORY_COLORS = ['#d58b4b', '#65a9b8', '#7da76a', '#a580bd', '#c56f72', '#6e91c9']

export function bodyModeFor(schema: NodeTypeSchema): NodeBodyMode {
  if (schema.display === 'preview') return schema.parameters.length > 0 ? 'parameters+viewer' : 'viewer'
  if (schema.display === 'text' || schema.display === 'selectionSet') return 'custom'
  if (schema.parameters.length > 0) return 'parameters'
  return 'none'
}

export function categoryColor(category: string): string {
  let hash = 0
  for (const character of category) hash = (hash * 31 + character.charCodeAt(0)) >>> 0
  return CATEGORY_COLORS[hash % CATEGORY_COLORS.length]
}

export function nodeDisplayName(schema: NodeTypeSchema, nickname?: string): string {
  return nickname?.trim() || schema.label
}
