import type { GraphParameter, GraphValue, NodeTypeSchema } from '../../types'
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

/**
 * The one node colour ramp.
 *
 * ⚠️ ONE ARRAY, TWO READERS, AND THAT IS THE POINT. `categoryColor` hashes into it
 * and the inspector's swatch row offers it. The inspector used to carry its own
 * eight-colour copy, which had already drifted to different hues - so a node given
 * a "green" by hand did not match the green the canvas assigned its category, and
 * neither followed the theme.
 *
 * Desaturated on purpose: these sit on a light grey canvas as a 2px strip beside a
 * dozen neighbours, so they have to read as one family. Separated in lightness as
 * well as hue, so the distinction survives the common colour-vision deficiencies.
 */
export const NODE_SWATCHES = ['#7d94ae', '#7fa08d', '#a8906f', '#9d8aa8', '#6f9aa8', '#a88a8a'] as const

/**
 * Where a node's preview is drawn: in its own viewport, in Archicad, or both.
 *
 * ⚠️ READ OFF THE `previewTarget` WIDGET, NOT OFF THE NODE TYPE. The widget enum
 * is the only thing a client may dispatch on - recognising the Preview node by
 * its id is exactly the branch that enum exists to remove, and it would silently
 * stop working the day the node is renamed.
 *
 * ⚠️ AND AN UNKNOWN TARGET DRAWS. A graph saved by a later build naming a fourth
 * target should show its geometry rather than hide it: a viewport that silently
 * shows nothing is indistinguishable from a node that produced nothing, and the
 * user would debug the wrong half. The native side takes the same view; see
 * PreviewTargetDrawsInArchicad.
 */
export type PreviewTarget = 'node' | 'archicad' | 'both'

export function previewTargetOf(schema: NodeTypeSchema, parameters: GraphParameter[]): PreviewTarget | undefined {
  const descriptor = schema.parameters.find((parameter) => parameter.ui?.widget === 'previewTarget')
  if (descriptor === undefined) return undefined
  const stored = parameters.find((parameter) => parameter.parameterId === descriptor.parameterId)
  const text = stored?.value?.text ?? descriptor.defaultValue?.text
  return text === 'node' || text === 'archicad' ? text : 'both'
}

export function bodyModeFor(schema: NodeTypeSchema): NodeBodyMode {
  if (schema.display === 'preview') return schema.parameters.length > 0 ? 'parameters+viewer' : 'viewer'
  if (schema.display === 'text' || schema.display === 'selectionSet' || schema.display === 'script') return 'custom'
  // INPUTS count as well as parameters. An input with nothing wired to it takes
  // a typed-in value - the runtime stores it as a parameter under the port's own
  // id - so a node like Multiply, which declares no parameters at all, still
  // needs the row body that has somewhere to type. The bare port pills are for
  // node types that have nothing to fill in.
  if (schema.parameters.length > 0 || schema.inputs.length > 0) return 'parameters'
  return 'none'
}

export function categoryColor(category: string): string {
  let hash = 0
  for (const character of category) hash = (hash * 31 + character.charCodeAt(0)) >>> 0
  return NODE_SWATCHES[hash % NODE_SWATCHES.length]
}

/**
 * A port's colour, by the KIND OF VALUE it carries.
 *
 * ⚠️ THE NUB'S COLOUR IS THE TYPE, NOT THE NODE. It used to inherit the node's
 * category colour, which meant two ports on the same node looked identical and
 * a number and a mesh looked the same wherever they came from - so the one
 * thing a user needs to see before dragging a wire (will these two connect?) was
 * the one thing the colour did not say. Families share a colour on purpose:
 * every geometric value is one hue, because "is this geometry" is the question
 * being asked at a glance, not "is this a polyline or a mesh".
 *
 * Chosen to stay apart at 11px on both the dark and the light canvas, and each
 * pair differs in lightness as well as hue so the distinction survives the
 * common colour-vision deficiencies. The nub is never the ONLY channel: the
 * port's label and its hover card both name the type in words.
 */
const PORT_COLORS: Record<string, string> = {
  double: '#3f83c4',
  integer: '#3f83c4',
  bool: '#c2352c',
  string: '#c08a2a',
  point3: '#c25f8c',
  polyline: '#c25f8c',
  polygon: '#c25f8c',
  mesh: '#c25f8c',
  archicadElementRef: '#3f9c8e',
  list: '#8a76b8',
}

/** The grey a port of no declared type gets - `absent` means "any type" here. */
/**
 * What a node's viewport draws with when the node has been given no colour.
 *
 * Here rather than inline in `NodeBody.svelte` because it is a VALUE, not a
 * theme token: it is the default of a per-node property the user can change,
 * and it belongs beside the other node-colour literals that this module owns.
 */
export const DEFAULT_VIEWER_COLOR = '#75c695'

export const ANY_PORT_COLOR = '#8a8a8a'

export function portColor(valueType: string): string {
  return PORT_COLORS[valueType] ?? ANY_PORT_COLOR
}

/**
 * How much data a port carries: one thing, a list of things, or a tree of them.
 *
 * ⚠️ THIS IS THE OTHER HALF OF THE NUB, AND IT IS NOT DECORATION. A structure
 * mismatch is the failure that bites hardest during quick edits - a list dropped
 * onto an item input silently iterates, a tree flattens, and the graph produces
 * a confidently wrong answer rather than an error. Grasshopper's answer is to
 * draw it on the port itself, and it is drawn WHETHER OR NOT anything is wired,
 * so the check happens before the drag rather than after the result looks odd.
 *
 * Drawn on both sides on purpose, even though the reference image marks only
 * outputs: the mismatch is between what one port PRODUCES and what the other
 * EXPECTS, and showing one side of a comparison is showing none of it.
 *
 * The shapes follow Grasshopper's, because they are the ones a user of that
 * tool already reads: one outline, a doubled outline, a dashed doubled outline.
 */
export type PortStructure = 'item' | 'list' | 'tree'

/**
 * The structure of a value the runtime actually produced.
 *
 * The catalog's encoding expands ONE level - a list's items arrive as leaves
 * that still carry their own valueType - which is exactly enough to tell a list
 * from a tree without shipping the whole value. A list whose first items are
 * themselves lists is a tree; a list of anything else is a list.
 */
export function structureOfValue(value: GraphValue | undefined): PortStructure | undefined {
  if (value === undefined || value.valueType !== 'list') return value === undefined ? undefined : 'item'
  const items = value.items ?? []
  return items.some((item) => item.valueType === 'list') ? 'tree' : 'list'
}

/**
 * What a port shows.
 *
 * A produced value wins, because it is what the port really carries. With no
 * run yet, the DECLARED type answers instead - a `list` output, or an input that
 * accepts multiple connections - so a freshly placed node already shows its
 * shape rather than waiting for an evaluation to admit it.
 */
export function portStructure(
  port: { valueType: string; acceptsMultiple?: boolean },
  value?: GraphValue,
): PortStructure {
  return structureOfValue(value) ?? (port.valueType === 'list' || port.acceptsMultiple === true ? 'list' : 'item')
}

/** The structure, in words, for the hover card - colour and shape are never the only channel. */
export function describeStructure(structure: PortStructure): string {
  if (structure === 'tree') return 'tree of lists'
  if (structure === 'list') return 'list'
  return 'single item'
}

export function nodeDisplayName(schema: NodeTypeSchema, nickname?: string): string {
  return nickname?.trim() || schema.label
}
