export interface PortSchema {
  portId: string
  label: string
  valueType: string
  required?: boolean
  acceptsMultiple?: boolean
}

export type PortDirection = 'input' | 'output'

export interface PortTransform {
  type: 'reverse' | 'simplify' | 'flatten' | 'graft' | 'reparameterize'
}
