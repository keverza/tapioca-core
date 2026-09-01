import assert from 'node:assert/strict'
import test from 'node:test'
import { filterValueTree, nodeValueTree, summarizeValue, valueNode } from '../src/nodes/types/valueTree.ts'
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
