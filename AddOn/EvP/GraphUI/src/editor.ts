import type { Connection, Edge, Node } from '@xyflow/svelte'
import type { NodeTypeSchema, PositionStore, SchemaNodeData } from './types'

export type DetailLevel = 'compact' | 'normal' | 'detailed'
export type ThemeMode = 'light' | 'dark' | 'system'

export const NODE_DRAG_MIME = 'application/svelteflow'

export function filterCatalog(catalog: NodeTypeSchema[], query: string): NodeTypeSchema[] {
  const tokens = query.trim().toLocaleLowerCase().split(/\s+/).filter(Boolean)
  if (tokens.length === 0) return catalog

  return catalog.filter((schema) => {
    const text = [
      schema.label,
      schema.nodeType,
      schema.category,
      schema.description,
      ...schema.inputs.flatMap((port) => [port.label, port.valueType]),
      ...schema.outputs.flatMap((port) => [port.label, port.valueType]),
    ]
      .join(' ')
      .toLocaleLowerCase()
    return tokens.every((token) => text.includes(token))
  })
}

export function groupCatalog(catalog: NodeTypeSchema[]): Map<string, NodeTypeSchema[]> {
  const groups = new Map<string, NodeTypeSchema[]>()
  for (const schema of catalog) {
    const items = groups.get(schema.category) ?? []
    items.push(schema)
    groups.set(schema.category, items)
  }
  return groups
}

export function isCatalogConnectionValid(
  connection: Connection | Edge,
  nodes: Node<SchemaNodeData>[],
  edges: Edge[],
): boolean {
  if (
    typeof connection.sourceHandle !== 'string' ||
    typeof connection.targetHandle !== 'string' ||
    connection.source === connection.target
  ) {
    return false
  }

  const source = nodes.find((node) => node.id === connection.source)
  const target = nodes.find((node) => node.id === connection.target)
  const output = source?.data.schema.outputs.find((port) => port.portId === connection.sourceHandle)
  const input = target?.data.schema.inputs.find((port) => port.portId === connection.targetHandle)
  if (output === undefined || input === undefined) return false

  const compatible = input.valueType === 'absent' || output.valueType === input.valueType
  if (!compatible) return false

  if (input.acceptsMultiple === true) return true
  return !edges.some(
    (edge) => edge.target === connection.target && edge.targetHandle === connection.targetHandle,
  )
}

export function detailLevelForZoom(zoom: number, current: DetailLevel): DetailLevel {
  if (current === 'compact' && zoom < 0.58) return 'compact'
  if (current === 'detailed' && zoom > 1.08) return 'detailed'
  if (zoom < 0.5) return 'compact'
  if (zoom > 1.2) return 'detailed'
  return 'normal'
}

export function initialTheme(storage: Pick<Storage, 'getItem'> | undefined): ThemeMode {
  const value = storage?.getItem('tapioca.graph.theme')
  return value === 'light' || value === 'dark' || value === 'system' ? value : 'system'
}

// ---------------------------------------------------------------------------
// Workflow library helpers.
//
// Here rather than in library.ts because this module imports nothing at runtime
// and is therefore the half the offline test runner can load. library.ts is the
// bridge-calling half.
// ---------------------------------------------------------------------------

/** How node positions cross to the runtime, which carries them and reads none of them. */
export interface LayoutField {
  key: string
  value: string
}

export interface LayoutRecord {
  nodeId: string
  fields: LayoutField[]
}

/** The runtime's own name rule, applied here so a bad name is refused before a round trip. */
export function isValidGraphName(name: string): boolean {
  if (name.length === 0 || name.length > 128) return false
  if (name.startsWith('.') || name.includes('..')) return false
  return /^[A-Za-z0-9._-]+$/.test(name)
}

export function describeNameRule(): string {
  return 'Letters, digits, dot, dash and underscore only, and it cannot start with a dot.'
}

export function layoutFromPositions(positions: PositionStore, nodeIds: Iterable<string>): LayoutRecord[] {
  const records: LayoutRecord[] = []
  for (const nodeId of nodeIds) {
    const position = positions.get(nodeId)
    if (position === undefined) continue
    records.push({
      nodeId,
      fields: [
        { key: 'x', value: String(Math.round(position.x)) },
        { key: 'y', value: String(Math.round(position.y)) },
      ],
    })
  }
  return records
}

export function applyLayoutToPositions(layout: LayoutRecord[], positions: PositionStore): void {
  for (const record of layout) {
    const x = Number(record.fields.find((field) => field.key === 'x')?.value)
    const y = Number(record.fields.find((field) => field.key === 'y')?.value)
    // A layout round-tripped from another client may carry fields this build
    // does not know. Anything that is not a finite pair is left to the automatic
    // placement rather than dropped on the origin.
    if (Number.isFinite(x) && Number.isFinite(y)) positions.set(record.nodeId, { x, y })
  }
}
