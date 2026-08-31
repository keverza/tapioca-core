export interface ComponentMessage {
  severity: 'info' | 'warning' | 'error'
  code: string
  title: string
  detail?: string
  nodeId: string
  portId?: string
}
