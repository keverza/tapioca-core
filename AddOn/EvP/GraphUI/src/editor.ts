import type { Connection, Edge, Node } from '@xyflow/svelte'
import type { GraphValue, NodeOutputRecord, NodeTypeSchema, PositionStore, SchemaNodeData } from './types'

export type DetailLevel = 'compact' | 'normal' | 'detailed'
export type ThemeMode = 'light' | 'dark' | 'system'

export const NODE_DRAG_MIME = 'application/svelteflow'
export const REFERENCE_EDGE_COLOR = '#8a8f98'
export const REFERENCE_EDGE_STYLE = `stroke: ${REFERENCE_EDGE_COLOR}; stroke-width: 1.4; stroke-dasharray: 5 5;`

export function displayedOutputText(output: NodeOutputRecord | undefined): string {
  if (output === undefined) return 'Not evaluated'
  if (output.value.valueType === 'string' && output.text === '') return '""'
  return output.text || output.summary || '—'
}

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

  if (!portTypesConnect(output.valueType, input.valueType)) return false

  if (input.acceptsMultiple === true) return true
  return !edges.some(
    (edge) => edge.target === connection.target && edge.targetHandle === connection.targetHandle,
  )
}

/**
 * Whether an edge may join these two ports.
 *
 * ⚠️ THIS MIRRORS `PortTypesConnect` IN GraphEdit.cpp, AND THE RUNTIME IS THE
 * AUTHORITY. The editor needs the answer before it has asked anyone - a wire is
 * validated while it is being dragged - which is the only reason a second copy
 * exists at all. When they disagree the visible symptom is nasty and silent: a
 * connection the runtime would happily accept simply refuses to be drawn, and
 * nothing anywhere says why. Change them together.
 *
 * - `absent` is the WILDCARD ON BOTH ENDS. On an input it means "any value". On
 *   an output it means "whatever I was given" - a `tree.graft` does not change
 *   what its items are and cannot know what they are.
 * - `integer` WIDENS to `double`, and the runtime converts the values as it
 *   gathers them. The reverse is refused on purpose: 2.5 is 2 or 3 depending on
 *   an answer only the author has, so `math.toInteger` is where they give it.
 */
export function portTypesConnect(output: string, input: string): boolean {
  if (input === 'absent' || output === 'absent' || output === input) return true
  return output === 'integer' && input === 'double'
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

// ---------------------------------------------------------------------------
// Typed-in values.
// ---------------------------------------------------------------------------

/**
 * What a person typed, in the runtime's value encoding - or `undefined` when
 * the text is not a value of that type at all.
 *
 * Refusing here rather than sending it is the point: the runtime would reject a
 * mistyped parameter anyway, and a round trip that comes back "parameter type
 * mismatch" tells the user less than the box that would not take the text.
 *
 * An EMPTY box is `undefined` too, and means "leave it alone". The native
 * setParam encoding has no spelling for an absent value, so there is nothing
 * honest to send for a cleared box; wiring an edge is how an internalised input
 * is overridden.
 */
export function graphValueFromText(valueType: string, text: string): GraphValue | undefined {
  const trimmed = text.trim()
  if (trimmed === '') return undefined

  if (valueType === 'double') {
    const number = Number(trimmed)
    return Number.isFinite(number) ? { valueType, number } : undefined
  }
  if (valueType === 'integer') {
    const number = Number(trimmed)
    return Number.isInteger(number) ? { valueType, number } : undefined
  }
  if (valueType === 'bool') {
    const lowered = trimmed.toLocaleLowerCase()
    if (['true', 'yes', 'on', '1'].includes(lowered)) return { valueType, bool: true }
    if (['false', 'no', 'off', '0'].includes(lowered)) return { valueType, bool: false }
    return undefined
  }
  if (valueType === 'string') return { valueType, text: trimmed }
  if (valueType === 'archicadElementRef') return { valueType, text: trimmed }
  if (valueType === 'point3') {
    const numbers = trimmed.split(/[\s,]+/).map(Number)
    return numbers.length === 3 && numbers.every(Number.isFinite) ? { valueType, numbers } : undefined
  }
  // list, mesh, polyline, polygon: produced by a node, never typed.
  return undefined
}

/** What a box of this type will accept, for the message when it did not. */
export function describeValueRule(valueType: string): string {
  if (valueType === 'double') return 'a number'
  if (valueType === 'integer') return 'a whole number'
  if (valueType === 'bool') return 'true or false'
  if (valueType === 'string') return 'any text'
  if (valueType === 'point3') return 'three numbers, e.g. "0, 0, 1"'
  if (valueType === 'archicadElementRef') return 'an element GUID'
  return `no typed value - ${valueType} comes from a connection`
}
