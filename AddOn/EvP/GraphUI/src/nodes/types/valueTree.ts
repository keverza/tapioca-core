import type { GraphParameter, GraphValue, NodeOutputRecord } from '../../types'

/**
 * One row of the property browser.
 *
 * Built from what the runtime actually sent - its self-describing values - and
 * nothing else. The browser therefore cannot show a field the graph does not
 * have, which is the difference between browsing a node and describing what a
 * node might one day contain.
 */
export interface ValueNode {
  /** Stable across rebuilds, so an expanded row stays expanded. */
  id: string
  label: string
  /** The runtime's own type name, e.g. `archicadElementRef`. */
  typeLabel: string
  /** The value on one line, for the row's right-hand column. */
  summary: string
  children: ValueNode[]
}

/** A leaf value on one line. Lists are described by shape, not by contents. */
export function summarizeValue(value: GraphValue | undefined): string {
  if (value === undefined) return '-'
  if (value.valueType === 'absent') return '(absent)'
  if (value.text !== undefined) return value.text
  if (value.bool !== undefined) return value.bool ? 'True' : 'False'
  if (value.number !== undefined) return String(value.number)
  if (value.numbers !== undefined) return `[${value.numbers.join(', ')}]`
  if (value.valueType === 'list' || value.items !== undefined) {
    const count = value.itemCount ?? value.items?.length ?? 0
    return `${count} item${count === 1 ? '' : 's'}`
  }
  return value.valueType
}

export function valueNode(id: string, label: string, value: GraphValue | undefined): ValueNode {
  const children: ValueNode[] = (value?.items ?? []).map((item, index) =>
    valueNode(`${id}.${index}`, `[${index}]`, item),
  )
  // The runtime truncates a big list rather than shipping it, and says so. The
  // row that is missing is shown as missing: a browser that silently ended at
  // item 24 would read as a list of 24.
  const declared = value?.itemCount ?? children.length
  if (value?.truncated === true || declared > children.length) {
    const hidden = Math.max(0, declared - children.length)
    children.push({
      id: `${id}.truncated`,
      label: `${hidden} more`,
      typeLabel: 'truncated',
      summary: 'The runtime capped this list; evaluate a narrower selection to see the rest.',
      children: [],
    })
  }
  return {
    id,
    label,
    typeLabel: value?.valueType ?? 'absent',
    summary: summarizeValue(value),
    children,
  }
}

/**
 * The node's own data, as two roots: what is STORED on it (its parameters, which
 * is where a selection set keeps its elements) and what it last PRODUCED.
 *
 * Two roots rather than one flat list because they answer different questions -
 * "what is this node holding" and "what did it just publish" - and a browser
 * that mixed them would make a stale output look like stored state.
 */
export function nodeValueTree(
  parameters: GraphParameter[],
  outputs: NodeOutputRecord[] = [],
): ValueNode[] {
  const roots: ValueNode[] = []
  if (parameters.length > 0) {
    roots.push({
      id: 'stored',
      label: 'Object structure',
      typeLabel: `${parameters.length} field${parameters.length === 1 ? '' : 's'}`,
      summary: '',
      children: parameters.map((parameter) =>
        valueNode(`stored.${parameter.parameterId}`, parameter.parameterId, parameter.value),
      ),
    })
  }
  if (outputs.length > 0) {
    roots.push({
      id: 'outputs',
      label: 'Conversions',
      typeLabel: `${outputs.length} output${outputs.length === 1 ? '' : 's'}`,
      summary: '',
      children: outputs.map((output) => {
        const node = valueNode(`outputs.${output.portId}`, output.portId, output.value)
        // The runtime already rendered this one; prefer its wording to ours.
        return { ...node, summary: output.summary || node.summary }
      }),
    })
  }
  return roots
}

/** Keep only the rows whose label, type or value matches, and their ancestors. */
export function filterValueTree(nodes: ValueNode[], query: string): ValueNode[] {
  const needle = query.trim().toLocaleLowerCase()
  if (needle === '') return nodes
  const kept: ValueNode[] = []
  for (const node of nodes) {
    const children = filterValueTree(node.children, query)
    const self = `${node.label} ${node.typeLabel} ${node.summary}`.toLocaleLowerCase().includes(needle)
    // A matching branch keeps ALL of its children: having found the row you
    // searched for, you want to see what is inside it.
    if (self) kept.push(node)
    else if (children.length > 0) kept.push({ ...node, children })
  }
  return kept
}
