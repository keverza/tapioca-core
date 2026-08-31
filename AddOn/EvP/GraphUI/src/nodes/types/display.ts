import type { NodeTypeSchema } from '../../types'
import type { NodeBodyMode } from './node'

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
