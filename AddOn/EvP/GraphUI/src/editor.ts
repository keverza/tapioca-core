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

/** What the runtime answers with after a transaction that added nodes. */
export interface AssignedNode {
  alias: string
  nodeId: string
}

/**
 * Move a plan's alias-keyed metadata onto the ids the runtime chose.
 *
 * ⚠️ AN ALIAS THE RESPONSE DOES NOT MENTION IS DROPPED, NOT GUESSED. Keeping
 * layout under a name no node has is how an editor comes to hold positions for
 * nodes that do not exist; if the runtime named fewer nodes than the plan asked
 * for, the honest thing is to have less layout, not invented layout.
 */
export function rekeyByAssignment<T>(
  byAlias: ReadonlyMap<string, T>,
  assigned: readonly AssignedNode[],
): Map<string, T> {
  const ids = new Map(assigned.map((entry) => [entry.alias, entry.nodeId]))
  const result = new Map<string, T>()
  for (const [alias, value] of byAlias) {
    const nodeId = ids.get(alias)
    if (nodeId !== undefined) result.set(nodeId, value)
  }
  return result
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
  /**
   * ⚠️ NO `nodeId`. THE RUNTIME NAMES NODES.
   *
   * A browser that invents ids is a second authority on identity: its scheme is
   * a rule the document cannot enforce, and two editors on one graph would
   * eventually collide. `alias` is a name local to THIS transaction - other
   * edits in the same batch use it to refer to a node whose real id does not
   * exist yet, and the response says what each one was actually called.
   */
  alias: string
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
  /**
   * ⚠️ KEYED BY ALIAS, NOT BY NODE ID, because the ids do not exist yet - the
   * runtime assigns them and answers with them. The caller rekeys these onto the
   * assigned ids once the transaction is accepted, which is also the point at
   * which the nodes are real enough to have layout at all.
   */
  positions: Map<string, XY>
  /** Alias -> the presentation copied from the original. */
  visuals: Map<string, NodeVisualState>
  /** Old id -> the alias standing in for its copy. */
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
  newAlias: (nodeType: string) => string,
): DuplicationPlan {
  const plan: DuplicationPlan = { edits: [], positions: new Map(), visuals: new Map(), renames: new Map() }
  if (clipboard.nodes.length === 0) return plan

  const origin = topLeft(clipboard.nodes)
  for (const node of clipboard.nodes) {
    const alias = newAlias(node.nodeType)
    plan.renames.set(node.nodeId, alias)
    plan.positions.set(alias, {
      x: at.x + (node.position.x - origin.x),
      y: at.y + (node.position.y - origin.y),
    })
    if (node.visual !== undefined) plan.visuals.set(alias, { ...node.visual })
    const edit: AddNodeEdit = { editKind: 'addNode', alias, nodeType: node.nodeType }
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

// ---------------------------------------------------------------------------
// PROMOTION.
//
// ⚠️ A PROMOTED PROPERTY IS AN ORDINARY NODE, DOCKED. Promoting `Height` on a
// Walls container adds an `archicad.element.setting` node, wires it from the
// host's element output, and records in LAYOUT that it should be drawn as a row
// under its host rather than as a box of its own. There is no second data model
// for promoted rows and therefore no conversion between them: "Convert to
// explicit node" clears one layout field, which is why it cannot lose a
// downstream link.
//
// See §45 of HANDOFF-TAPIOCA-GraphUI-Property-Browser.md for why the additive
// instance-port design this replaced does not work.
//
// ⚠️ AND THE DOCK IS LAYOUT, NOT DOCUMENT. Whether a node is drawn as a row or
// as a box changes nothing about what the graph computes, so it lives in the
// same per-node metadata as positions - which the runtime round-trips and never
// reads. Putting it in the document would give the evaluator an opinion about
// drawing.
// ---------------------------------------------------------------------------

/** The node type a promotion creates. The runtime's id, not a label. */
export const ELEMENT_SETTING_NODE_TYPE = 'archicad.element.setting'

export interface DockState {
  /** The node this row is drawn under. */
  dockedTo: string
  /** Where among its host's rows. Ties break on node id, so order is total. */
  dockOrder: number
}

export interface PromotionPlan {
  edits: (AddNodeEdit | ConnectEdit)[]
  /** The transaction-local name of the node being added, for rekeying. */
  alias: string
  /** Keyed by alias, rekeyed onto the assigned id once the runtime answers. */
  docks: Map<string, DockState>
}

/**
 * The edits that promote one setting off `hostNodeId`.
 *
 * ⚠️ ONE TRANSACTION, ADD AND WIRE TOGETHER. A promotion that added the node
 * and then connected it in a second call could leave a graph holding a setting
 * node wired to nothing - a node the user did not ask for and cannot explain -
 * if the second call failed or the app closed between them. `alias` is what
 * makes the wire nameable before the node has an id; see AddNodeEdit.
 *
 * `elementType` is stored on the node rather than inferred at run time. A
 * promotion is made against ONE type's schema, and rewiring the host to another
 * type has to be visible rather than silently answering anyway.
 */
export function promotionPlan(
  hostNodeId: string,
  hostOutputPort: string,
  settingId: string,
  elementType: string,
  dockOrder: number,
  newAlias: (nodeType: string) => string,
): PromotionPlan {
  const alias = newAlias(ELEMENT_SETTING_NODE_TYPE)
  const plan: PromotionPlan = { edits: [], alias, docks: new Map() }

  plan.edits.push({
    editKind: 'addNode',
    alias,
    nodeType: ELEMENT_SETTING_NODE_TYPE,
    parameters: [
      { parameterId: 'setting', value: { valueType: 'string', text: settingId } },
      { parameterId: 'elementType', value: { valueType: 'string', text: elementType } },
    ],
  })
  plan.edits.push({
    editKind: 'connect',
    sourceNode: hostNodeId,
    sourcePort: hostOutputPort,
    targetNode: alias,
    targetPort: 'elements',
  })
  plan.docks.set(alias, { dockedTo: hostNodeId, dockOrder })
  return plan
}

/**
 * The dock a node's layout fields describe, or `undefined` for a node drawn as
 * an ordinary box.
 *
 * ⚠️ A MISSING OR UNPARSEABLE ORDER DOCKS THE ROW AT THE END RATHER THAN
 * UNDOCKING IT. A graph written by a client that records the host but not the
 * order is still a graph whose author meant that node to be a row; dropping the
 * dock over a bad number would silently scatter a node's promoted rows across
 * the canvas, which looks like data loss and is much harder to explain than a
 * row in the wrong place.
 */
export function dockFromFields(fields: readonly { key: string; value: string }[]): DockState | undefined {
  const dockedTo = fields.find((field) => field.key === 'dockedTo')?.value ?? ''
  if (dockedTo === '') return undefined
  const order = Number(fields.find((field) => field.key === 'dockOrder')?.value)
  return { dockedTo, dockOrder: Number.isFinite(order) ? order : Number.MAX_SAFE_INTEGER }
}

export function dockToFields(dock: DockState): { key: string; value: string }[] {
  return [
    { key: 'dockedTo', value: dock.dockedTo },
    { key: 'dockOrder', value: String(dock.dockOrder) },
  ]
}

/**
 * The docked rows of each host, in the order they should be drawn.
 *
 * ⚠️ A ROW WHOSE HOST IS NOT IN `nodeIds` IS RETURNED UNDOCKED, not hidden. A
 * dock pointing at a deleted node would otherwise make its row vanish from the
 * canvas while still being in the document and still evaluating - a node the
 * user cannot see, cannot select and cannot delete. Orphans become ordinary
 * boxes, which is visible and fixable.
 */
export function dockedRows(
  docks: ReadonlyMap<string, DockState>,
  nodeIds: Iterable<string>,
): { byHost: Map<string, string[]>; orphans: string[] } {
  const present = new Set(nodeIds)
  const byHost = new Map<string, string[]>()
  const orphans: string[] = []

  const ordered = [...docks.entries()]
    .filter(([nodeId]) => present.has(nodeId))
    // Node id breaks a tie, so two rows promoted in the same gesture do not
    // swap places between repaints.
    .sort((a, b) => a[1].dockOrder - b[1].dockOrder || a[0].localeCompare(b[0]))

  for (const [nodeId, dock] of ordered) {
    if (!present.has(dock.dockedTo) || dock.dockedTo === nodeId) {
      orphans.push(nodeId)
      continue
    }
    const rows = byHost.get(dock.dockedTo)
    if (rows === undefined) byHost.set(dock.dockedTo, [nodeId])
    else rows.push(nodeId)
  }
  return { byHost, orphans }
}

/**
 * The next free row index under a host. Appends rather than renumbering, so
 * promoting a fourth property does not move the first three.
 */
export function nextDockOrder(docks: ReadonlyMap<string, DockState>, hostNodeId: string): number {
  let next = 0
  for (const dock of docks.values())
    if (dock.dockedTo === hostNodeId) next = Math.max(next, dock.dockOrder + 1)
  return next
}
