import type { XYPosition } from '@xyflow/svelte'
import type { NodeVisualState } from './nodes/types/node'
import type { PortSchema } from './nodes/types/port'
import type { ExecutionMode, RuntimeStatus } from './nodes/types/runtime'

export type { NodeBodyMode, NodeVisualState } from './nodes/types/node'
export type { PortDirection, PortSchema, PortTransform } from './nodes/types/port'
export type { ExecutionMode, RuntimeStatus } from './nodes/types/runtime'

export interface ParameterSchema {
  parameterId: string
  label: string
  valueType: string
  required: boolean
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
  /** Presentation metadata. It is never sent to the evaluator. */
  visual?: NodeVisualState
  onvisualchange?: (nodeId: string, visual: NodeVisualState) => void
  onexecutionchange?: (nodeId: string, mode: ExecutionMode) => void
}

export type PositionStore = Map<string, XYPosition>
