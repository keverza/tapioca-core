import type { XYPosition } from '@xyflow/svelte'

export interface PortSchema {
  portId: string
  label: string
  valueType: string
  required?: boolean
  acceptsMultiple?: boolean
}

export interface ParameterSchema {
  parameterId: string
  label: string
  valueType: string
  required: boolean
}

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
  display?: 'ports' | 'text' | 'preview'
  generations?: string[]
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
  status: 'dirty' | 'complete' | 'failed' | 'blocked' | 'cancelled' | 'skipped'
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

export interface SchemaNodeData extends Record<string, unknown> {
  schema: NodeTypeSchema
  parameters: GraphParameter[]
  result?: NodeResultRecord
}

export type PositionStore = Map<string, XYPosition>
