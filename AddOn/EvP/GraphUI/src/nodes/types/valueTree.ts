import type { GraphParameter, GraphValue, NodeOutputRecord } from '../../types'
import {
  geometryTotals,
  isEmptyGeometry,
  viewerBounds,
  viewerGeometryFrom,
  type GeometryTotals,
} from '../viewer/geometry.ts'

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
  /**
   * The output port this row IS, when it is one.
   *
   * ⚠️ SET ONLY ON A WHOLE OUTPUT, never on a row inside one. A port reference
   * addresses `node.port` and nothing finer, so a Copy button on a `[3]` row
   * would hand over a reference to the entire list while appearing to name one
   * item - a wire that looks like it carries one wall and carries forty.
   */
  portId?: string
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

/** Numbers a person reads: three decimals, and no trailing noise. */
function metres(value: number): string {
  return `${Number(value.toFixed(3))} m`
}

function coordinates(point: { x: number; y: number; z: number }): string {
  return `${Number(point.x.toFixed(3))}, ${Number(point.y.toFixed(3))}, ${Number(point.z.toFixed(3))}`
}

function fact(id: string, label: string, typeLabel: string, summary: string): ValueNode {
  return { id, label, typeLabel, summary, children: [] }
}

function describeConstruction(totals: GeometryTotals): string {
  const parts: string[] = []
  if (totals.points > 0) parts.push(`${totals.points} point${totals.points === 1 ? '' : 's'}`)
  if (totals.curves > 0) parts.push(`${totals.curves} curve${totals.curves === 1 ? '' : 's'}`)
  if (totals.meshes > 0) parts.push(`${totals.meshes} mesh${totals.meshes === 1 ? '' : 'es'}`)
  return parts.join(', ')
}

/**
 * WHAT THIS VALUE IS MADE OF, AND HOW BIG IT IS.
 *
 * ⚠️ READ THROUGH THE SAME MODULE THE VIEWPORT DRAWS FROM. A browser saying a
 * mesh has 4,608 triangles while the viewer draws something else would be two
 * answers to one question, and the browser is the one a user would believe. So
 * these come from viewerGeometryFrom rather than from a second walk of the value.
 *
 * ⚠️ AND WHAT DID NOT ARRIVE IS SAID, NOT COUNTED AS ZERO. A mesh past the
 * bridge's encoding cap crosses as counts with no vertices; reporting its size as
 * 0 x 0 x 0 would be a confident wrong answer about a real shape.
 *
 * Empty for a value with no geometry in it at all - a number, a string, an
 * element reference - because a Geometry branch on a Number node is a row that
 * exists only to say there is nothing here.
 */
export function geometryFacts(id: string, value: GraphValue | undefined): ValueNode[] {
  if (value === undefined) return []
  const geometry = viewerGeometryFrom([value])
  if (isEmptyGeometry(geometry)) {
    if (!geometry.truncated) return []
    return [
      fact(
        `${id}.geometry`,
        'Geometry',
        'not sent',
        'The runtime capped this value, so its geometry did not cross the bridge.',
      ),
    ]
  }

  const totals = geometryTotals(geometry)
  const bounds = viewerBounds(geometry)

  const construction: ValueNode[] = []
  if (totals.points > 0) construction.push(fact(`${id}.geometry.points`, 'Points', 'integer', String(totals.points)))
  if (totals.curves > 0) {
    construction.push(fact(`${id}.geometry.curves`, 'Curves', 'integer', String(totals.curves)))
    // ⚠️ THE TESSELLATION'S LENGTH, which is what the graph produced. An arc
    // arrives as a polyline; quoting an analytic length would quote a number the
    // runtime never computed.
    construction.push(fact(`${id}.geometry.length`, 'Curve length', 'double', metres(totals.length)))
  }
  if (totals.meshes > 0) {
    construction.push(fact(`${id}.geometry.meshes`, 'Meshes', 'integer', String(totals.meshes)))
    construction.push(fact(`${id}.geometry.triangles`, 'Triangles', 'integer', String(totals.triangles)))
  }
  construction.push(fact(`${id}.geometry.vertices`, 'Vertices', 'integer', String(totals.vertices)))

  const branches: ValueNode[] = [
    {
      id: `${id}.geometry`,
      label: 'Construction',
      typeLabel: 'geometry',
      summary: describeConstruction(totals),
      children: construction,
    },
  ]

  if (bounds !== null) {
    const dx = bounds.max.x - bounds.min.x
    const dy = bounds.max.y - bounds.min.y
    const dz = bounds.max.z - bounds.min.z
    branches.push({
      id: `${id}.size`,
      label: 'Size',
      typeLabel: 'bounds',
      summary: `${metres(dx)} x ${metres(dy)} x ${metres(dz)}`,
      children: [
        fact(`${id}.size.x`, 'Size X', 'double', metres(dx)),
        fact(`${id}.size.y`, 'Size Y', 'double', metres(dy)),
        fact(`${id}.size.z`, 'Size Z', 'double', metres(dz)),
        fact(`${id}.size.min`, 'Min', 'point3', coordinates(bounds.min)),
        fact(`${id}.size.max`, 'Max', 'point3', coordinates(bounds.max)),
        fact(
          `${id}.size.centre`,
          'Centre',
          'point3',
          coordinates({
            x: (bounds.min.x + bounds.max.x) / 2,
            y: (bounds.min.y + bounds.max.y) / 2,
            z: (bounds.min.z + bounds.max.z) / 2,
          }),
        ),
      ],
    })
  }

  if (geometry.truncated) {
    // Part of the value arrived and part did not, so every number above is a
    // LOWER BOUND. Saying so is the difference between a partial answer and a
    // wrong one.
    branches.push(
      fact(
        `${id}.geometry.capped`,
        'Capped',
        'truncated',
        'Some of this value did not cross the bridge, so the counts above are a lower bound.',
      ),
    )
  }
  return branches
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
        // Facts FIRST, contents after. "What is this and how big is it" is the
        // question somebody opens a browser to answer, and it should not sit
        // below four hundred list rows.
        const facts = geometryFacts(`outputs.${output.portId}`, output.value)
        return {
          ...node,
          // The runtime already rendered this one; prefer its wording to ours.
          summary: output.summary || node.summary,
          // ONLY HERE: this row IS the port, so a reference copied from it
          // addresses exactly what it names. See ValueNode.portId.
          portId: output.portId,
          children: [...facts, ...node.children],
        }
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

/**
 * The tree as lines, indented and tab-separated: `label`, `typeLabel`, `summary`.
 *
 * Takes the tree it is GIVEN, which is the filtered one, so "Copy all" copies
 * what the panel shows. Copying the unfiltered tree from a panel displaying four
 * rows would hand over four hundred - technically more helpful, and not what the
 * button appeared to offer.
 */
export function flattenValueTree(nodes: ValueNode[], depth = 0): string[] {
  const lines: string[] = []
  for (const node of nodes) {
    lines.push(`${'  '.repeat(depth)}${node.label}\t${node.typeLabel}\t${node.summary}`)
    lines.push(...flattenValueTree(node.children, depth + 1))
  }
  return lines
}
