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
  inputs: PortSchema[]
  outputs: PortSchema[]
  parameters: ParameterSchema[]
}

export interface GraphParameter {
  parameterId: string
  valueType: string
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
  nodes: GraphNodeRecord[]
  edges: GraphEdgeRecord[]
}

export interface NodeResultRecord {
  nodeId: string
  status: 'dirty' | 'complete' | 'failed' | 'blocked'
  message: string
  durationMilliseconds: number
  itemCount: number
  previewAvailable: boolean
}

export interface SchemaNodeData extends Record<string, unknown> {
  schema: NodeTypeSchema
  parameters: GraphParameter[]
  result?: NodeResultRecord
}

export type PositionStore = Map<string, XYPosition>
