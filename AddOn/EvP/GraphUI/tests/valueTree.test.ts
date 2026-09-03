import assert from 'node:assert/strict'
import test from 'node:test'
import {
  filterValueTree,
  flattenValueTree,
  geometryFacts,
  nodeValueTree,
  summarizeValue,
  valueNode,
} from '../src/nodes/types/valueTree.ts'
import type { GraphParameter, NodeOutputRecord } from '../src/types.ts'

const elements: GraphParameter = {
  parameterId: 'elements',
  value: {
    valueType: 'list',
    itemCount: 2,
    items: [
      { valueType: 'archicadElementRef', text: 'GUID-A' },
      { valueType: 'archicadElementRef', text: 'GUID-B' },
    ],
  },
}

test('a value renders on one line by its own declared type', () => {
  assert.equal(summarizeValue(undefined), '-')
  assert.equal(summarizeValue({ valueType: 'absent' }), '(absent)')
  assert.equal(summarizeValue({ valueType: 'double', number: 6.5 }), '6.5')
  assert.equal(summarizeValue({ valueType: 'bool', bool: false }), 'False')
  assert.equal(summarizeValue({ valueType: 'point3', numbers: [0, 0, 1] }), '[0, 0, 1]')
  assert.equal(summarizeValue({ valueType: 'list', itemCount: 1, items: [] }), '1 item')
  assert.equal(summarizeValue({ valueType: 'mesh' }), 'mesh')
})

test('a list becomes a branch whose children carry stable ids', () => {
  const node = valueNode('stored.elements', 'elements', elements.value)
  assert.equal(node.typeLabel, 'list')
  assert.equal(node.summary, '2 items')
  assert.deepEqual(node.children.map((child) => child.id), ['stored.elements.0', 'stored.elements.1'])
  assert.equal(node.children[0].summary, 'GUID-A')
})

test('a list the runtime capped says so instead of ending early', () => {
  const node = valueNode('stored.big', 'big', {
    valueType: 'list',
    itemCount: 500,
    truncated: true,
    items: [{ valueType: 'double', number: 1 }],
  })
  assert.equal(node.children.length, 2)
  assert.equal(node.children[1].label, '499 more')
  assert.equal(node.children[1].typeLabel, 'truncated')
})

test('stored fields and published outputs are separate roots', () => {
  const outputs: NodeOutputRecord[] = [
    { portId: 'value', value: { valueType: 'list', itemCount: 2, items: [] }, text: '', summary: 'List of 2' },
  ]
  const roots = nodeValueTree([elements], outputs)
  assert.deepEqual(roots.map((root) => root.id), ['stored', 'outputs'])
  // The runtime already worded this one; its wording wins over ours.
  assert.equal(roots[1].children[0].summary, 'List of 2')

  // A node with neither has no tree at all, rather than two empty headings.
  assert.deepEqual(nodeValueTree([], []), [])
})

test('filtering keeps matching rows and the branches that lead to them', () => {
  const roots = nodeValueTree([elements], [])
  assert.deepEqual(filterValueTree(roots, ''), roots)

  const found = filterValueTree(roots, 'guid-b')
  assert.equal(found.length, 1)
  assert.deepEqual(found[0].children[0].children.map((child) => child.summary), ['GUID-B'])

  // A branch that matches on its own keeps everything inside it.
  assert.equal(filterValueTree(roots, 'elements')[0].children[0].children.length, 2)
  assert.deepEqual(filterValueTree(roots, 'nothing-here'), [])
})


// ---------------------------------------------------------------------------
// What a value is MADE OF and HOW BIG it is.
//
// ⚠️ READ THROUGH THE VIEWER'S OWN MODULE, so the browser and the viewport
// cannot disagree about one value. These tests are mostly about the two ways a
// browser lies: reporting a size for geometry that never crossed the bridge, and
// offering a port reference from a row that is not a port.
// ---------------------------------------------------------------------------

// A unit box: eight corners, twelve triangles.
const boxMesh = {
  valueType: 'mesh',
  numbers: [0, 0, 0, 2, 0, 0, 2, 3, 0, 0, 3, 0, 0, 0, 1, 2, 0, 1, 2, 3, 1, 0, 3, 1],
  indices: [0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7],
}

test('a mesh reports what builds it and how big it is', () => {
  const facts = geometryFacts('out', boxMesh)

  const construction = facts.find((node) => node.label === 'Construction')
  assert.ok(construction)
  assert.equal(construction.summary, '1 mesh')
  const rows = new Map(construction.children.map((child) => [child.label, child.summary]))
  assert.equal(rows.get('Meshes'), '1')
  assert.equal(rows.get('Triangles'), '4')
  assert.equal(rows.get('Vertices'), '8')

  const size = facts.find((node) => node.label === 'Size')
  assert.ok(size)
  // 2 x 3 x 1, from the corners above - the MAIN SIZES a user opens this to read.
  assert.equal(size.summary, '2 m x 3 m x 1 m')
  const extent = new Map(size.children.map((child) => [child.label, child.summary]))
  assert.equal(extent.get('Size X'), '2 m')
  assert.equal(extent.get('Size Z'), '1 m')
  assert.equal(extent.get('Min'), '0, 0, 0')
  assert.equal(extent.get('Centre'), '1, 1.5, 0.5')
})

test('a curve reports its length along the points that arrived', () => {
  // ⚠️ THE TESSELLATION'S LENGTH. An arc reaches the browser as a polyline, and
  // quoting an analytic length would quote a number the runtime never computed.
  const facts = geometryFacts('out', { valueType: 'polyline', numbers: [0, 0, 0, 3, 0, 0, 3, 4, 0] })
  const construction = facts.find((node) => node.label === 'Construction')
  assert.ok(construction)
  const rows = new Map(construction.children.map((child) => [child.label, child.summary]))
  assert.equal(rows.get('Curves'), '1')
  assert.equal(rows.get('Curve length'), '7 m')

  // A polygon closes, so its closing span counts.
  const closed = geometryFacts('out', { valueType: 'polygon', numbers: [0, 0, 0, 3, 0, 0, 3, 4, 0] })
  const closedRows = new Map(
    closed.find((node) => node.label === 'Construction')!.children.map((child) => [child.label, child.summary]),
  )
  assert.equal(closedRows.get('Curve length'), '12 m')
})

test('geometry that did not cross the bridge is said, never sized', () => {
  // ⚠️ THE LIE THIS PREVENTS. A mesh past the encoding cap arrives as counts
  // with no vertices; reporting 0 m x 0 m x 0 m would be a confident wrong answer
  // about a real shape, and it is the browser a user would believe.
  const facts = geometryFacts('out', { valueType: 'mesh', itemCount: 900000, truncated: true })
  assert.equal(facts.length, 1)
  assert.equal(facts[0].typeLabel, 'not sent')
  assert.equal(facts.find((node) => node.label === 'Size'), undefined)
})

test('a partially truncated value says its counts are a lower bound', () => {
  const facts = geometryFacts('out', {
    valueType: 'list',
    itemCount: 400,
    truncated: true,
    items: [{ valueType: 'point3', numbers: [1, 2, 3] }],
  })
  assert.ok(facts.find((node) => node.label === 'Size'))
  assert.ok(facts.find((node) => node.label === 'Capped'))
})

test('a value with no geometry gets no geometry rows at all', () => {
  // A Geometry branch on a Number node is a row that exists only to say there is
  // nothing here.
  assert.deepEqual(geometryFacts('out', { valueType: 'double', number: 4 }), [])
  assert.deepEqual(geometryFacts('out', { valueType: 'string', text: 'hello' }), [])
  assert.deepEqual(geometryFacts('out', undefined), [])
})

test('only a whole output carries a port reference, and its facts come first', () => {
  const output: NodeOutputRecord = { portId: 'shape', summary: 'Mesh', value: boxMesh }
  const roots = nodeValueTree([], [output])
  const port = roots[0].children[0]

  // ⚠️ THE ROW THAT IS THE PORT, AND ONLY IT. A reference addresses
  // `node.port` and nothing finer, so a Copy button on an item row would hand
  // over the whole output while appearing to name one item.
  assert.equal(port.portId, 'shape')
  for (const child of port.children) assert.equal(child.portId, undefined)

  // "What is this and how big" is what somebody opens a browser to answer, so it
  // is not below four hundred list rows.
  assert.equal(port.children[0].label, 'Construction')
  assert.equal(port.children[1].label, 'Size')
})

test('a stored parameter is never offered as a port reference', () => {
  // Parameters are not ports. A reference to one would not resolve to anything.
  const roots = nodeValueTree([elements], [])
  assert.equal(roots[0].children[0].portId, undefined)
})

test('copy-all flattens what is on screen, indented and tab separated', () => {
  const lines = flattenValueTree([
    { id: 'a', label: 'outputs', typeLabel: '1 output', summary: '', children: [
      { id: 'a.b', label: 'shape', typeLabel: 'mesh', summary: 'Mesh', children: [] },
    ] },
  ])
  assert.deepEqual(lines, ['outputs\t1 output\t', '  shape\tmesh\tMesh'])
})

// ---- Tree shape ------------------------------------------------------------
//
// Twelve walls flat and four walls on each of three storeys are the same
// `value`. If the browser cannot tell them apart, the tree layer is invisible
// to the only place a user could ever see it.

const branchedOutput: NodeOutputRecord = {
  portId: 'walls',
  value: { valueType: 'list', itemCount: 3, items: [] },
  text: '',
  summary: '3 items',
  itemType: 'archicadElementRef',
  branchCount: 2,
  branchesTruncated: false,
  branches: [
    {
      path: '{0;0}',
      segments: [0, 0],
      itemCount: 2,
      truncated: false,
      value: {
        valueType: 'list',
        itemCount: 2,
        items: [
          { valueType: 'archicadElementRef', text: 'GUID-A' },
          { valueType: 'archicadElementRef', text: 'GUID-B' },
        ],
      },
    },
    {
      path: '{0;1}',
      segments: [0, 1],
      itemCount: 1,
      truncated: false,
      value: { valueType: 'list', itemCount: 1, items: [{ valueType: 'archicadElementRef', text: 'GUID-C' }] },
    },
  ],
}

test('a branched output is browsed by its paths, and the port keeps its item type', () => {
  const port = nodeValueTree([], [branchedOutput])[0].children[0]
  assert.equal(port.typeLabel, 'archicadElementRef')
  assert.deepEqual(port.children.map((child) => child.label), ['{0;0}', '{0;1}'])
  assert.deepEqual(port.children.map((child) => child.summary), ['2 items', '1 item'])
  // The items are still reachable, one level further in.
  assert.deepEqual(port.children[0].children.map((child) => child.summary), ['GUID-A', 'GUID-B'])
})

test('one branch is shown flat, because a {0} row would appear on every scalar', () => {
  const single: NodeOutputRecord = {
    ...branchedOutput,
    branchCount: 1,
    branches: [branchedOutput.branches![0]],
  }
  const port = nodeValueTree([], [single])[0].children[0]
  // The flat `value` renders, not a lone branch row.
  assert.equal(port.children.every((child) => child.typeLabel !== 'branch'), true)
})

test('an output from a runtime with no tree layer renders exactly as before', () => {
  const legacy: NodeOutputRecord = {
    portId: 'value',
    value: { valueType: 'list', itemCount: 2, items: [{ valueType: 'double', number: 1 }, { valueType: 'double', number: 2 }] },
    text: '',
    summary: 'List of 2',
  }
  const port = nodeValueTree([], [legacy])[0].children[0]
  assert.deepEqual(port.children.map((child) => child.summary), ['1', '2'])
})

test('branches the runtime capped are said to be missing, not silently absent', () => {
  const capped: NodeOutputRecord = { ...branchedOutput, branchCount: 130, branchesTruncated: true }
  const port = nodeValueTree([], [capped])[0].children[0]
  const last = port.children[port.children.length - 1]
  assert.equal(last.label, '128 more branches')
  assert.equal(last.typeLabel, 'truncated')
})

test('a capped branch reports its true length rather than what arrived', () => {
  const capped: NodeOutputRecord = {
    ...branchedOutput,
    branches: [{ ...branchedOutput.branches![0], itemCount: 400, truncated: true }, branchedOutput.branches![1]],
  }
  const branch = nodeValueTree([], [capped])[0].children[0].children[0]
  assert.equal(branch.summary, '400 items')
  assert.equal(branch.children[branch.children.length - 1].label, '398 more')
})
