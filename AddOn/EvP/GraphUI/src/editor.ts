import type { Connection, Edge, Node } from '@xyflow/svelte'
import type { GraphParameter, GraphValue, NodeOutputRecord, PositionStore, SchemaNodeData } from './types'
import type { NodeVisualState } from './nodes/types/node'

export type DetailLevel = 'compact' | 'normal' | 'detailed'
export type ThemeMode = 'light' | 'dark' | 'system'

/**
 * The wire shape, in ONE place.
 *
 * ⚠️ TWO CALL SITES BUILD EDGES - the reconciler that rebuilds every wire from
 * native state, and the optimistic-preview path a fresh connection takes - and a
 * literal in each is how they come to disagree. They were both 'smoothstep'; a
 * third site added later would have had to guess.
 *
 * Bezier rather than smoothstep because the tangent leaves a port along the
 * handle's own direction, which reads as "this comes out of THAT nub" at the
 * density where wires bunch. Smoothstep's right angles look tidy on a sparse
 * graph and become an unreadable ladder on a busy one.
 */
export const GRAPH_EDGE_TYPE = 'bezier'

export const REFERENCE_EDGE_COLOR = '#8a8f98'
export const REFERENCE_EDGE_STYLE = `stroke: ${REFERENCE_EDGE_COLOR}; stroke-width: 1.4; stroke-dasharray: 5 5;`

export function displayedOutputText(output: NodeOutputRecord | undefined): string {
  if (output === undefined) return 'Not evaluated'
  if (output.value.valueType === 'string' && output.text === '') return '""'
  return output.text || output.summary || '—'
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

/** The undo depths offered, and the one in force when nothing was chosen. */
export const UNDO_DEPTH_CHOICES = [10, 20, 50, 100] as const
export const DEFAULT_UNDO_DEPTH = 20

/**
 * ⚠️ THE RUNTIME CLAMPS THIS, AND ITS ANSWER WINS. What is stored here is a
 * REQUEST; `GraphSetHistoryDepth` returns the depth actually in force, and the
 * editor shows that. A stored number the runtime refused would otherwise be
 * displayed as the setting forever.
 */
export function initialUndoDepth(storage: Pick<Storage, 'getItem'> | undefined): number {
  const stored = Number(storage?.getItem('tapioca.graph.undoDepth'))
  return Number.isInteger(stored) && stored >= 1 && stored <= 200 ? stored : DEFAULT_UNDO_DEPTH
}

/* --- The undo timeline ----------------------------------------------------
 *
 * ⚠️ HALF OF WHAT A USER CALLS AN EDIT IS NOT IN THE DOCUMENT. The runtime owns
 * nodes, edges and parameters, and its undo stack is snapshots of exactly those.
 * A node's POSITION is not among them, and deliberately so: the evaluator never
 * reads a coordinate, and putting layout in the semantic document would make
 * every move a graph change, dirty the nodes, and re-run the graph for a drag.
 *
 * But `Ctrl+Z` is a promise about what the user just did, not about which side
 * of the bridge stored it. So the editor keeps ONE timeline whose entries are
 * either a marker for a runtime step or a metadata step of its own, and undo
 * walks that timeline: a marker calls `GraphUndo`, a metadata step is reversed
 * here, and either way the metadata that belongs with that moment is restored.
 *
 * The alternative - two independent stacks, Ctrl+Z hitting whichever - undoes
 * things in an order the user never performed them in, which is worse than not
 * undoing moves at all.
 */

export interface MetadataSnapshot {
  positions: ReadonlyMap<string, XY>
  visuals: ReadonlyMap<string, NodeVisualState>
}

export interface UndoStep {
  /** `native`: the runtime holds this step. `editor`: this module does. */
  kind: 'native' | 'editor'
  label: string
  /** Consecutive editor steps sharing a non-empty key fold into one. */
  coalesceKey: string
  before: MetadataSnapshot
  after: MetadataSnapshot
}

/** A snapshot that shares nothing with the live maps it was taken from. */
export function snapshotMetadata(
  positions: ReadonlyMap<string, XY>,
  visuals: ReadonlyMap<string, NodeVisualState>,
): MetadataSnapshot {
  return {
    positions: new Map([...positions].map(([id, at]) => [id, { x: at.x, y: at.y }])),
    visuals: new Map([...visuals].map(([id, visual]) => [id, { ...visual }])),
  }
}

/** Whether two snapshots would look any different on the canvas. */
export function metadataChanged(a: MetadataSnapshot, b: MetadataSnapshot): boolean {
  return JSON.stringify(serializeSnapshot(a)) !== JSON.stringify(serializeSnapshot(b))
}

function serializeSnapshot(snapshot: MetadataSnapshot): unknown {
  const positions = [...snapshot.positions].sort(([a], [b]) => (a < b ? -1 : 1))
  const visuals = [...snapshot.visuals].sort(([a], [b]) => (a < b ? -1 : 1))
  return { positions, visuals }
}

/**
 * Add a step, honouring coalescing and the depth in force.
 *
 * ⚠️ THE TIMELINE IS TRIMMED TO THE SAME DEPTH THE RUNTIME KEEPS, FROM THE SAME
 * END. If it were longer, `Ctrl+Z` would eventually reach a marker for a runtime
 * step the runtime has already dropped and refuse with "nothing to undo" while
 * still showing an enabled Undo.
 */
export function pushUndoStep(stack: readonly UndoStep[], step: UndoStep, depth: number): UndoStep[] {
  const last = stack[stack.length - 1]
  if (
    step.kind === 'editor' &&
    step.coalesceKey !== '' &&
    last !== undefined &&
    last.kind === 'editor' &&
    last.coalesceKey === step.coalesceKey
  ) {
    // Fold: the earlier entry already holds the state from before the gesture.
    return [...stack.slice(0, -1), { ...last, after: step.after }]
  }
  const grown = [...stack, step]
  return grown.length > depth ? grown.slice(grown.length - depth) : grown
}

/**
 * How many undoable steps the runtime recorded between two readings of its
 * history.
 *
 * Driven off `stepsRecorded` rather than `undoCount` because a step recorded
 * when the stack is already full leaves the count unchanged, and a coalesced
 * edit does too. See `HistoryState::stepsRecorded` in the runtime.
 */
export function nativeStepsSince(previous: number, current: number): number {
  return current > previous ? current - previous : 0
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

/* --- Copy, duplicate and paste -------------------------------------------
 *
 * The policy lives here, as pure functions over plain data, because the rules
 * below are the whole feature and every one of them is a case somebody will
 * later be tempted to "simplify".
 */

/** One copied node: its semantics, where it sat, and how it looked. */
export interface ClipboardNode {
  /**
   * The ORIGINAL id.
   *
   * ⚠️ NOT THE ID ANYTHING WILL BE PASTED UNDER. It is kept only so the copied
   * edges, which name the originals, can be rewritten onto the new ids. Pasting
   * under this id would collide with the node still on the canvas.
   */
  nodeId: string
  nodeType: string
  parameters: GraphParameter[]
  position: XY
  /** Nickname, colour, view mode, port layout, size, display state. */
  visual?: NodeVisualState
}

export interface ClipboardEdge {
  sourceNode: string
  sourcePort: string
  targetNode: string
  targetPort: string
}

export interface GraphClipboard {
  version: 1
  nodes: ClipboardNode[]
  edges: ClipboardEdge[]
}

export const CLIPBOARD_VERSION = 1

/** Where a pasted copy lands when the pointer cannot say. */
export const PASTE_OFFSET = 28

export interface XY {
  x: number
  y: number
}

/** One `addNode` edit, in the shape `Tapioca.GraphApplyEdits` accepts. */
export interface AddNodeEdit {
  editKind: 'addNode'
  nodeId: string
  nodeType: string
  parameters?: GraphParameter[]
}

export interface ConnectEdit {
  editKind: 'connect'
  sourceNode: string
  sourcePort: string
  targetNode: string
  targetPort: string
}

export interface DuplicationPlan {
  edits: (AddNodeEdit | ConnectEdit)[]
  /** New id -> where it goes. The runtime never reads a position; the editor does. */
  positions: Map<string, XY>
  /** New id -> the presentation copied from the original. */
  visuals: Map<string, NodeVisualState>
  /** Old id -> new id, so a caller can select what it just pasted. */
  renames: Map<string, string>
}

/**
 * Collect the selection into a clipboard payload.
 *
 * ⚠️ AN EDGE IS COPIED ONLY WHEN BOTH ENDS ARE. A wire from outside the
 * selection into it is dropped rather than reconnected to the original's
 * upstream. Silently wiring a copy to the source's inputs is the behaviour that
 * makes people stop trusting duplicate: the copy looks independent and is not.
 * GH2 reaches the same conclusion and puts reconnection behind an explicit flag.
 */
export function collectClipboard(
  selectedNodeIds: readonly string[],
  nodes: readonly Node<SchemaNodeData>[],
  edges: readonly ClipboardEdge[],
  positions: PositionStore,
  visuals: ReadonlyMap<string, NodeVisualState>,
): GraphClipboard {
  const wanted = new Set(selectedNodeIds)
  const copied: ClipboardNode[] = []
  for (const node of nodes) {
    if (!wanted.has(node.id)) continue
    copied.push({
      nodeId: node.id,
      nodeType: node.data.schema.nodeType,
      parameters: node.data.parameters ?? [],
      position: positions.get(node.id) ?? node.position ?? { x: 0, y: 0 },
      visual: visuals.get(node.id),
    })
  }
  const present = new Set(copied.map((node) => node.nodeId))
  return {
    version: CLIPBOARD_VERSION,
    nodes: copied,
    edges: edges.filter((edge) => present.has(edge.sourceNode) && present.has(edge.targetNode)),
  }
}

/** The selection's bounding-box top-left, which is what `at` positions. */
function topLeft(nodes: readonly ClipboardNode[]): XY {
  return {
    x: Math.min(...nodes.map((node) => node.position.x)),
    y: Math.min(...nodes.map((node) => node.position.y)),
  }
}

/**
 * Turn a clipboard into one transaction.
 *
 * Every node is added, THEN every internal wire is connected: a `connect` naming
 * a node the same batch has not added yet would be refused, and the runtime
 * applies a batch in order.
 *
 * ⚠️ RELATIVE GEOMETRY IS PRESERVED, ABSOLUTE POSITION IS NOT. The copies are
 * translated so the selection's top-left lands on `at`, which is what makes a
 * pasted cluster keep its shape instead of collapsing onto one point.
 */
export function duplicationPlan(
  clipboard: GraphClipboard,
  at: XY,
  newId: (nodeType: string) => string,
): DuplicationPlan {
  const plan: DuplicationPlan = { edits: [], positions: new Map(), visuals: new Map(), renames: new Map() }
  if (clipboard.nodes.length === 0) return plan

  const origin = topLeft(clipboard.nodes)
  for (const node of clipboard.nodes) {
    const id = newId(node.nodeType)
    plan.renames.set(node.nodeId, id)
    plan.positions.set(id, {
      x: at.x + (node.position.x - origin.x),
      y: at.y + (node.position.y - origin.y),
    })
    if (node.visual !== undefined) plan.visuals.set(id, { ...node.visual })
    const edit: AddNodeEdit = { editKind: 'addNode', nodeId: id, nodeType: node.nodeType }
    // Omitted rather than sent empty: the schema accepts no `parameters` at all,
    // and an empty array is a claim that the node had none rather than that none
    // were copied.
    if (node.parameters.length > 0) edit.parameters = node.parameters.map((parameter) => ({ ...parameter }))
    plan.edits.push(edit)
  }

  for (const edge of clipboard.edges) {
    const source = plan.renames.get(edge.sourceNode)
    const target = plan.renames.get(edge.targetNode)
    // Defensive: collectClipboard already dropped these. A clipboard that came
    // from somewhere else may not have.
    if (source === undefined || target === undefined) continue
    plan.edits.push({
      editKind: 'connect',
      sourceNode: source,
      sourcePort: edge.sourcePort,
      targetNode: target,
      targetPort: edge.targetPort,
    })
  }
  return plan
}

/**
 * Read a clipboard payload that may have come from anywhere.
 *
 * ⚠️ RETURNS UNDEFINED RATHER THAN THROWING. The OS clipboard can hold anything
 * at all, and a paste of somebody's shopping list must be a status message
 * rather than a broken editor.
 */
export function parseClipboard(text: string): GraphClipboard | undefined {
  let parsed: unknown
  try {
    parsed = JSON.parse(text)
  } catch {
    return undefined
  }
  if (typeof parsed !== 'object' || parsed === null) return undefined
  const candidate = parsed as Partial<GraphClipboard>
  if (candidate.version !== CLIPBOARD_VERSION) return undefined
  if (!Array.isArray(candidate.nodes) || !Array.isArray(candidate.edges)) return undefined
  const nodes: ClipboardNode[] = []
  for (const node of candidate.nodes) {
    if (
      typeof node?.nodeId !== 'string' ||
      typeof node?.nodeType !== 'string' ||
      typeof node?.position?.x !== 'number' ||
      typeof node?.position?.y !== 'number'
    )
      return undefined
    nodes.push({
      nodeId: node.nodeId,
      nodeType: node.nodeType,
      parameters: Array.isArray(node.parameters) ? node.parameters : [],
      position: { x: node.position.x, y: node.position.y },
      visual: node.visual,
    })
  }
  const edges: ClipboardEdge[] = []
  for (const edge of candidate.edges) {
    if (
      typeof edge?.sourceNode !== 'string' ||
      typeof edge?.sourcePort !== 'string' ||
      typeof edge?.targetNode !== 'string' ||
      typeof edge?.targetPort !== 'string'
    )
      return undefined
    edges.push(edge)
  }
  return { version: CLIPBOARD_VERSION, nodes, edges }
}

/**
 * A stable key that folds one continuous gesture into one undo step.
 *
 * ⚠️ THE GESTURE TOKEN IS WHAT MAKES THIS CORRECT. Keying only on the parameter
 * would collapse two SEPARATE drags of the same slider into a single Ctrl+Z,
 * because the runtime coalesces consecutive edits carrying the same key and has
 * no clock to tell the drags apart. The caller bumps the token when a gesture
 * ends.
 */
export function parameterGestureKey(nodeId: string, parameterId: string, gesture: number): string {
  return `setParam:${nodeId}:${parameterId}:${gesture}`
}

/* --- Placing things under the pointer -------------------------------------
 *
 * ⚠️ A POINT IS THE CENTRE OF WHAT IT PLACES, NOT ITS TOP-LEFT CORNER. Both the
 * node and the browser dialog used to be positioned by their top-left, so a
 * node appeared down and to the right of where it was asked for, and the dialog
 * dropped its whole 720px body below and right of the cursor. Neither is where
 * anybody pointed.
 */

/** What a freshly placed node occupies before its content has been measured. */
export const NOMINAL_NODE_SIZE = { width: 248, height: 96 }
export const NOMINAL_PREVIEW_NODE_SIZE = { width: 292, height: 220 }

export function nominalNodeSize(display: string | undefined): { width: number; height: number } {
  return display === 'preview' ? NOMINAL_PREVIEW_NODE_SIZE : NOMINAL_NODE_SIZE
}

/** Turn a point meant as a centre into the top-left the layout actually stores. */
export function centreOn(point: XY, size: { width: number; height: number }): XY {
  return { x: point.x - size.width / 2, y: point.y - size.height / 2 }
}

/**
 * The extent a clipboard's nodes will occupy once pasted.
 *
 * The spread between the top-left corners plus one node, because the position
 * of a node is its corner and the last one still has a body.
 */
export function clipboardExtent(clipboard: GraphClipboard): { width: number; height: number } {
  if (clipboard.nodes.length === 0) return { width: 0, height: 0 }
  const xs = clipboard.nodes.map((node) => node.position.x)
  const ys = clipboard.nodes.map((node) => node.position.y)
  return {
    width: Math.max(...xs) - Math.min(...xs) + NOMINAL_NODE_SIZE.width,
    height: Math.max(...ys) - Math.min(...ys) + NOMINAL_NODE_SIZE.height,
  }
}

/**
 * Where a paste's bounding box should start so the CLUSTER is centred on the
 * pointer rather than its first node.
 */
export function centredPasteAnchor(clipboard: GraphClipboard, centre: XY): XY {
  return centreOn(centre, clipboardExtent(clipboard))
}
