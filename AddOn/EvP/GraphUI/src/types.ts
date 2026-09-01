import type { XYPosition } from '@xyflow/svelte'
import type { NodeVisualState } from './nodes/types/node'
import type { PortSchema } from './nodes/types/port'
import type { ExecutionMode, RuntimeStatus } from './nodes/types/runtime'
import type { PortConnectionState } from './nodes/types/port'
import type { ComponentMessage } from './nodes/types/diagnostics'
import type { PortReference } from './nodes/types/portReference'

export type { NodeBodyMode, NodeVisualState } from './nodes/types/node'
export type { PortDirection, PortSchema, PortTransform } from './nodes/types/port'
export type { ExecutionMode, RuntimeStatus } from './nodes/types/runtime'
export type { PortReference } from './nodes/types/portReference'

/**
 * UI-1. How the runtime says a parameter is EDITED.
 *
 * A projection of the native `ParameterUi`, never a definition authored here:
 * `widget` is a closed enum the runtime validates at registration, not a Svelte
 * component name, so adding a node type or a parameter needs no change in this
 * package. See docs/architecture/api/HANDOFF-NodeGraphUIBuilder.md section 4.
 *
 * Every field is a DISPLAY or INPUT hint. None of it changes what the parameter
 * means: the node clamps and rounds its own value, so a control that ignored a
 * range would produce a wrong-looking box, never a wrong answer.
 */
export type ParameterWidget =
  | 'auto'
  | 'number'
  | 'slider'
  | 'boolean'
  | 'select'
  | 'text'
  | 'vector'
  | 'point'
  | 'color'
  | 'readOnly'
  | 'previewTarget'

/**
 * Where a select's choices come from when they are not literal.
 *
 * ⚠️ NOTHING HERE ENUMERATES A MODEL DOMAIN. The layer list belongs to the open
 * project, so it is neither static catalog data nor the browser's to compute -
 * the parameter names the domain and the native attribute listing answers with
 * the members.
 */
export type ParameterOptionSource =
  | 'none'
  | 'layer'
  | 'pen'
  | 'fill'
  | 'lineType'
  | 'surface'
  | 'buildingMaterial'
  | 'composite'
  | 'profile'

export interface ParameterOption {
  label: string
  value: GraphValue
}

export interface ParameterUi {
  widget: ParameterWidget
  section: string
  order: number
  help: string
  /** Drawn after the field - "mm". Presentation only; the value is in runtime units. */
  unit: string
  minimum?: number
  maximum?: number
  step?: number
  decimals?: number
  /**
   * A SIBLING PARAMETER supplying the live bound instead of the constant above.
   * This is what makes a slider whose range and precision the user controls
   * expressible without this package knowing which node it is looking at.
   */
  minimumParameter?: string
  maximumParameter?: string
  stepParameter?: string
  decimalsParameter?: string
  components: string[]
  options: ParameterOption[]
  optionSource: ParameterOptionSource
}

/**
 * An attribute's swatch, as a DEFINITION rather than an image.
 *
 * Eight bytes of bit pattern, a dash sequence, a stack of skin thicknesses: the
 * runtime sends what the attribute IS and the client draws it. See
 * AttributeSwatch.svelte for why that split is the useful one.
 */
export interface AttributePreview {
  kind: 'color' | 'pattern' | 'line' | 'composite' | 'surface'
  color?: string
  /** Surfaces only: whether the surface carries a texture image. */
  hasTexture?: boolean
  fillKind?: 'vector' | 'symbol' | 'solid' | 'empty' | 'linearGradient' | 'radialGradient' | 'image'
  /** Eight rows of eight bits, one byte each, high bit leftmost. */
  pattern?: number[]
  lineKind?: 'solid' | 'dashed' | 'symbol'
  dashes?: number[]
  thickness?: number
  skins?: { thickness: number; color?: string }[]
}

/** One row of the native attribute listing. */
export interface AttributeRow {
  label: string
  name?: string
  number?: number
  index: number
  color?: string
  hidden?: boolean
  locked?: boolean
  /** Slash-joined folder path; absent means the attribute sits at the root. */
  folder?: string
  preview?: AttributePreview
}

export interface ParameterSchema {
  parameterId: string
  label: string
  valueType: string
  required: boolean
  defaultValue?: GraphValue
  /** Absent is valid: every parameter registered before UI-1 has none. */
  ui?: ParameterUi
}

/**
 * Stage F3. One input forwarded to one output while the node is bypassed.
 *
 * Declared by the node TYPE, never inferred here: a node with two compatible
 * inputs has no derivable answer, and a client that guessed would make bypass
 * mean something different from what the runtime does.
 */
export interface BypassMapping {
  inputId: string
  outputId: string
}

/**
 * Stage F. What the user has told the graph to do with a node.
 *
 * Not a status. A mode is authored and persists with the graph; a status is
 * produced by a run and is session-only. See NodeResultRecord['status'].
 */
export interface NodeTypeSchema {
  nodeType: string
  label: string
  category: string
  description: string
  executionDomain: string
  effect?: string
  /**
   * How the runtime suggests this node's body is drawn: 'ports' (default),
   * 'text' (show the node's text output, the Grasshopper-panel shape), or
   * 'preview'. Driven by the catalog rather than by branching on nodeType here,
   * so a new inspectable node needs no change in this file.
   */
  display?: 'ports' | 'text' | 'preview' | 'selectionSet'
  generations?: string[]
  /**
   * Empty means this type cannot be bypassed. The picker and context menu grey
   * the action from this rather than attempting the edit and reporting the
   * refusal - an action offered and then refused is worse than one never
   * offered.
   */
  bypassMappings?: BypassMapping[]
  /** Whether ExecutionMode 'holding' is legal for this type. */
  holdCapable?: boolean
  inputs: PortSchema[]
  outputs: PortSchema[]
  parameters: ParameterSchema[]
}

/** The runtime's self-describing value encoding. */
export interface GraphValue {
  valueType: string
  bool?: boolean
  number?: number
  text?: string
  numbers?: number[]
  /**
   * A mesh's triangles, into `numbers` read as xyz triples. Present only on a
   * top-level mesh within the encoding cap; a mesh inside a list, or one past the
   * cap, arrives as counts with `truncated`.
   */
  indices?: number[]
  itemCount?: number
  truncated?: boolean
  items?: GraphValue[]
}

export interface GraphParameter {
  parameterId: string
  value?: GraphValue
  /** @deprecated the runtime still accepts this; new code sends `value`. */
  numberValue?: number
}

export interface GraphNodeRecord {
  nodeId: string
  nodeType: string
  /**
   * Stage F, and it arrives with the STATE rather than with the results, because
   * it is document state: a client that read it from the per-run results would
   * lose every disabled node the moment the run cache was dropped.
   */
  executionMode?: ExecutionMode
  parameters: GraphParameter[]
}

export interface GraphEdgeRecord {
  sourceNode: string
  sourcePort: string
  targetNode: string
  targetPort: string
}

export interface GraphState {
  revision: number
  graphId?: string
  lastRunId?: number
  lastEventSeq?: number
  nodes: GraphNodeRecord[]
  edges: GraphEdgeRecord[]
}

export interface NodeOutputRecord {
  portId: string
  value: GraphValue
  /** The value rendered by the runtime, ready to display. */
  text: string
  /** A short label: "List of 12", "Element", "Number". */
  summary: string
}

export interface NodeResultRecord {
  nodeId: string
  /**
   * Stage F1's public vocabulary. `dirty`, `complete`, `failed` and `skipped`
   * were the old internal spellings and are gone, not aliased.
   */
  status: RuntimeStatus
  /**
   * The stable machine-readable reason, e.g. `node.blocked.sideEffectsWithheld`.
   *
   * ⚠️ BRANCH ON THIS, NEVER ON `message`. Several distinct situations share one
   * status - a withheld side effect, an upstream disabled node and an unreleased
   * dam are all `blocked` - and only the code tells them apart. `message` is
   * prose for a person and may be reworded at any time.
   */
  code?: string
  message: string
  durationMilliseconds: number
  itemCount: number
  cacheHit?: boolean
  evaluationCount?: number
  runId?: number
  previewAvailable: boolean
  /** What the node actually produced. This is what makes results visible. */
  outputs?: NodeOutputRecord[]
}

/**
 * What one evaluation's levels actually overlapped.
 *
 * ADR-007's gate is that pure nodes DEMONSTRABLY execute concurrently, and
 * `peakConcurrency` is the demonstration: it is counted by the node bodies
 * themselves, not inferred from the timings, so it cannot be produced by fast
 * sequential execution.
 */
export interface ParallelismMetrics {
  workerThreads: number
  maxParallel: number
  peakConcurrency: number
  wallClockMs: number
  workMs: number
  /** Work divided by wall clock. 1.0 means nothing overlapped. */
  speedup: number
  levels: {
    levelIndex: number
    executedCount: number
    workerNodeCount: number
    hostNodeCount: number
    peakConcurrency: number
    wallClockMs: number
    workMs: number
  }[]
}

export interface EvaluationSummary {
  graphId: string
  runId: number
  revision: number
  succeeded: boolean
  cancelled: boolean
  error: string
  failedNode: string
  executedCount: number
  cacheHitCount: number
  failedCount: number
  blockedCount: number
  /**
   * Whether the run actually committed its host effects.
   *
   * ⚠️ FALSE IS NOT A FAILURE, AND MUST NOT BE READ AS SUCCESS EITHER. A run that
   * did not ask for side effects reports every effectful node as skipped and
   * still succeeds - which is exactly what every automatic pass does. Only a
   * deliberate press asks, and only then does this distinguish "sent" from
   * "the runtime declined to send".
   */
  effectsCommitted?: boolean
  /** The effectful nodes this run reported rather than performed. */
  skippedEffectNodes?: string[]
  parallelism?: ParallelismMetrics
}

export type SelectionAction = 'update' | 'add' | 'remove' | 'reselect' | 'clear'

export interface SelectionActionOutcome {
  ok: boolean
  error: string
  nodeId: string
  action: SelectionAction
  count: number
  changed: number
  missing: string[]
  revision: number
  evaluated: boolean
  executedCount: number
  /** Separate from `error`: the set can change correctly and the graph downstream still fail. */
  evaluationError: string
}

/**
 * One domain's whole answer.
 *
 * More than the rows, because a pen listing also carries the project's pen SETS
 * - the picker draws its set dropdown from the same response that carried the
 * pens, rather than from a second round trip that could disagree with it about
 * which set is showing.
 */
export interface AttributeListing {
  attributes: AttributeRow[]
  penSets?: string[]
  /** Which set these pens came from; absent means the project's current one. */
  penSet?: string
}

export interface SchemaNodeData extends Record<string, unknown> {
  schema: NodeTypeSchema
  parameters: GraphParameter[]
  /**
   * Stage F. Carried on the node rather than read from `result`, so a node stays
   * visibly disabled BETWEEN runs - when there is no result to read at all.
   */
  executionMode?: ExecutionMode
  result?: NodeResultRecord
  /**
   * Present on selection-set nodes. The node draws its own five buttons, so the
   * handler has to reach it - and it comes through `data` because that is the
   * only channel Svelte Flow gives a custom node.
   */
  onselectionaction?: (nodeId: string, action: SelectionAction) => void
  /** True while an action on THIS node is in flight. */
  selectionBusy?: boolean
  /**
   * Present on nodes that perform a host effect. Automatic evaluation never
   * commits one - `allowSideEffects` defaults to refused - so this press is the
   * deliberate act that does, and it is per node rather than graph-wide: two
   * effectful nodes in one graph are two decisions.
   */
  onexecute?: (nodeId: string) => void
  /** True while THIS node's effect is being committed. */
  executeBusy?: boolean
  /**
   * What this node's viewer should draw.
   *
   * ⚠️ RESOLVED BY THE EDITOR, NOT READ OFF THE NODE'S OWN RESULT, because a
   * Preview node has NO OUTPUTS - it is a terminal, and its geometry lives on the
   * node upstream of it. The editor follows the same wire the runtime's preview
   * projection does, so the node's viewport and the Archicad overlay cannot
   * disagree about what this node is showing.
   */
  viewerValues?: GraphValue[]
  /** Presentation metadata. It is never sent to the evaluator. */
  visual?: NodeVisualState
  onvisualchange?: (nodeId: string, visual: NodeVisualState) => void
  onexecutionchange?: (nodeId: string, mode: ExecutionMode) => void
  /**
   * A value typed into a control. The node hands over TEXT and the port's
   * declared type; turning that into the runtime's value encoding is the
   * editor's job, because only the editor knows what the runtime accepts.
   */
  onparameterchange?: (nodeId: string, parameterId: string, valueType: string, text: string) => void
  /** A port reference pasted onto one of this node's inputs: a connection request. */
  onportreference?: (reference: PortReference, target: { nodeId: string; portId: string }) => void
  oncopyportreference?: (nodeId: string, portId: string, direction: 'input' | 'output') => void
  onpasteportreference?: (nodeId: string, portId: string) => void
  /**
   * A right-click on one of this node's ports. The editor answers with THE
   * context menu; the port draws none of its own.
   */
  onportcontextmenu?: (
    event: MouseEvent,
    target: { nodeId: string; portId: string; direction: 'input' | 'output' },
  ) => void
  /**
   * What the native attribute listing answered, keyed by option source.
   *
   * Passed IN rather than fetched by the control, because a presentational
   * component must not call the bridge - and because one project-wide listing
   * serves every picker on the canvas instead of one call per node.
   *
   * The RAW rows, not finished options: turning a row into an option needs the
   * parameter's value type (a pen is picked by number, everything else by name),
   * and the row also carries the swatch, folder and layer flags the picker
   * draws. Converting here would throw all of that away.
   */
  attributeListings?: Record<string, AttributeListing>
  /** A select with nothing to show asking for its domain to be listed. */
  onrequestoptions?: (source: ParameterOptionSource, penSet?: string) => void
  portConnections?: PortConnectionState[]
  messages?: ComponentMessage[]
}

export type PositionStore = Map<string, XYPosition>
