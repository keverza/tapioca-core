export interface ComponentMessage {
  severity: 'info' | 'warning' | 'error'
  code: string
  title: string
  detail?: string
  nodeId: string
  portId?: string
}

export function aggregateSeverity(messages: ComponentMessage[]): ComponentMessage['severity'] | undefined {
  if (messages.some((message) => message.severity === 'error')) return 'error'
  if (messages.some((message) => message.severity === 'warning')) return 'warning'
  if (messages.some((message) => message.severity === 'info')) return 'info'
  return undefined
}

export function formatDiagnostics(messages: ComponentMessage[]): string {
  return messages.map((message) => `[${message.severity.toUpperCase()}] ${message.code} ${message.title}${message.portId ? ` / ${message.portId}` : ''}${message.detail ? `\n${message.detail}` : ''}`).join('\n\n')
}
