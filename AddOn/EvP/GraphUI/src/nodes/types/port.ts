export interface PortSchema {
  portId: string
  label: string
  valueType: string
  required?: boolean
  acceptsMultiple?: boolean
}

export type PortDirection = 'input' | 'output'
export type PortLayout = 'horizontal' | 'vertical'

export interface PortCapabilities {
  copyReference: boolean
  pasteReference: boolean
  internalise: boolean
  promote: boolean
  inspect: boolean
  transforms: PortTransform['type'][]
}

export interface PortDefinition extends PortSchema {
  nickname?: string
  capabilities: PortCapabilities
}

export interface PortTransform {
  type: 'reverse' | 'simplify' | 'flatten' | 'graft' | 'reparameterize'
}

export interface PortConnectionState {
  portId: string
  direction: PortDirection
  connected: boolean
  connectionCount: number
  peerLabels?: string[]
  transforms?: PortTransform[]
}
