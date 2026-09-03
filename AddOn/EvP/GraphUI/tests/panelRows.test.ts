import assert from 'node:assert/strict'
import test from 'node:test'
import { panelRows, panelStructure } from '../src/nodes/types/panelRows.ts'
import type { NodeOutputRecord } from '../src/types.ts'

/** The Panel's one output: the tree it was handed, as the runtime publishes it. */
function output(record: Partial<NodeOutputRecord>): NodeOutputRecord {
  return {
    portId: 'value',
    value: { valueType: 'list', itemCount: 0, items: [] },
    text: '',
    summary: '',
    ...record,
  }
}

const flatList = output({
  value: { valueType: 'list', itemCount: 2, items: [{ valueType: 'double', number: 2 }, { valueType: 'double', number: 3 }] },
  summary: 'List of 2',
  branchCount: 1,
  branches: [
    {
      path: '{0}',
      segments: [0],
      itemCount: 2,
      truncated: false,
      value: { valueType: 'list', itemCount: 2, items: [{ valueType: 'double', number: 2 }, { valueType: 'double', number: 3 }] },
    },
  ],
})

const grafted = output({
  summary: 'List of 2',
  branchCount: 2,
  branches: [
    {
      path: '{0;0}',
      segments: [0, 0],
      itemCount: 1,
      truncated: false,
      value: { valueType: 'list', itemCount: 1, items: [{ valueType: 'double', number: 2 }] },
    },
    {
      path: '{0;1}',
      segments: [0, 1],
      itemCount: 1,
      truncated: false,
      value: { valueType: 'list', itemCount: 1, items: [{ valueType: 'double', number: 3 }] },
    },
  ],
})

test('one branch at the root is a LIST, and draws no path header', () => {
  const rows = panelRows(flatList)
  assert.deepEqual(rows.map((row) => row.kind), ['item', 'item'])
  assert.deepEqual(rows.map((row) => (row.kind === 'item' ? row.text : '')), ['2', '3'])
  assert.equal(panelStructure(flatList), 'List of 2')
})

test('several branches are a TREE, and each one is announced by its path', () => {
  const rows = panelRows(grafted)
  assert.deepEqual(rows.map((row) => row.kind), ['path', 'item', 'path', 'item'])
  assert.deepEqual(
    rows.filter((row) => row.kind === 'path').map((row) => (row.kind === 'path' ? row.label : '')),
    ['{0;0}', '{0;1}'],
  )
  // The same items as the flat list above, and that is the point: only the
  // headers tell a grafted tree from a flattened one.
  assert.deepEqual(rows.filter((row) => row.kind === 'item').map((row) => (row.kind === 'item' ? row.text : '')), ['2', '3'])
  assert.equal(panelStructure(grafted), 'List of 2 in 2 branches')
})

test('the index restarts within each branch, as Grasshopper counts', () => {
  const rows = panelRows(grafted)
  assert.deepEqual(rows.filter((row) => row.kind === 'item').map((row) => (row.kind === 'item' ? row.index : -1)), [0, 0])
})

test('a single branch that is NOT the root still shows its path', () => {
  const shifted = output({
    summary: 'List of 1',
    branchCount: 1,
    branches: [
      {
        path: '{1}',
        segments: [1],
        itemCount: 1,
        truncated: false,
        value: { valueType: 'list', itemCount: 1, items: [{ valueType: 'double', number: 7 }] },
      },
    ],
  })
  assert.equal(panelRows(shifted)[0].kind, 'path')
})

test('what the runtime capped is said out loud, per branch and overall', () => {
  const capped = output({
    summary: 'List of 40',
    branchCount: 5,
    branchesTruncated: true,
    branches: [
      {
        path: '{0}',
        segments: [0],
        itemCount: 40,
        truncated: true,
        value: { valueType: 'list', itemCount: 2, items: [{ valueType: 'double', number: 1 }] },
      },
    ],
  })
  const notes = panelRows(capped).filter((row) => row.kind === 'note')
  assert.deepEqual(notes.map((row) => (row.kind === 'note' ? row.text : '')), [
    '39 more items not shown',
    '4 more branches not shown',
  ])
})

test('an empty branch says so rather than rendering as nothing at all', () => {
  const empty = output({
    summary: 'List of 0',
    branchCount: 1,
    branches: [
      { path: '{0}', segments: [0], itemCount: 0, truncated: false, value: { valueType: 'list', itemCount: 0, items: [] } },
    ],
  })
  assert.deepEqual(panelRows(empty).map((row) => (row.kind === 'note' ? row.text : '')), ['(empty branch)'])
})

test('a runtime that sends no branches keeps rendering its own text, one line per item', () => {
  const older = output({ text: '2\n3', summary: 'List of 2' })
  const rows = panelRows(older)
  assert.deepEqual(rows.map((row) => (row.kind === 'item' ? row.text : '')), ['2', '3'])
  // Told nothing false, only less: no shape is claimed.
  assert.equal(panelStructure(older), 'List of 2')
})

test('a panel with no result at all has no rows', () => {
  assert.deepEqual(panelRows(undefined), [])
  assert.equal(panelStructure(undefined), '')
})
