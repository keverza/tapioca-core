/**
 * The text a port's "Copy reference" puts on the clipboard.
 *
 * One shape, parsed by the same function that wrote it, because the two ends of
 * that round trip are a right-click on one node and a paste into another: a
 * hand-rolled reader on the paste side would drift from the writer the first
 * time a field was added.
 */
export interface PortReference {
  kind: 'nodePort'
  nodeId: string
  portId: string
  direction: 'input' | 'output'
  valueType: string
}

export function serializePortReference(reference: PortReference): string {
  return JSON.stringify(reference)
}

/**
 * `undefined` for anything that is not one of ours - which includes ordinary
 * text a person meant to type into the box, so a paste of "12" is a value and
 * never a failed reference.
 */
export function parsePortReference(text: string): PortReference | undefined {
  const trimmed = text.trim()
  if (!trimmed.startsWith('{')) return undefined
  let parsed: unknown
  try {
    parsed = JSON.parse(trimmed)
  } catch {
    return undefined
  }
  if (typeof parsed !== 'object' || parsed === null) return undefined
  const candidate = parsed as Partial<PortReference>
  if (candidate.kind !== 'nodePort') return undefined
  if (typeof candidate.nodeId !== 'string' || candidate.nodeId === '') return undefined
  if (typeof candidate.portId !== 'string' || candidate.portId === '') return undefined
  if (candidate.direction !== 'input' && candidate.direction !== 'output') return undefined
  return {
    kind: 'nodePort',
    nodeId: candidate.nodeId,
    portId: candidate.portId,
    direction: candidate.direction,
    valueType: typeof candidate.valueType === 'string' ? candidate.valueType : '',
  }
}
