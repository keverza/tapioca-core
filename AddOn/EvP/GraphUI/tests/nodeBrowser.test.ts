import assert from 'node:assert/strict'
import test from 'node:test'
import {
  ALL_CATEGORY,
  browserCategories,
  clampDialogPosition,
  moveSelection,
  rankCatalog,
  scoreSchema,
  subsequenceMatch,
} from '../src/browser/nodeBrowser.ts'
import type { NodeTypeSchema } from '../src/types.ts'

function schema(overrides: Partial<NodeTypeSchema> & Pick<NodeTypeSchema, 'nodeType' | 'label'>): NodeTypeSchema {
  return {
    category: 'Input',
    description: '',
    executionDomain: 'worker',
    inputs: [],
    outputs: [],
    parameters: [],
    ...overrides,
  }
}

const getSelection = schema({
  nodeType: 'archicad.getSelection',
  label: 'Get Selection',
  category: 'Archicad',
  description: 'The elements currently selected in Archicad',
})
const selectionFilter = schema({
  nodeType: 'archicad.filterSelection',
  label: 'Selection Filter',
  category: 'Archicad',
})
const number = schema({
  nodeType: 'number',
  label: 'Number',
  category: 'Input',
  description: 'A typed-in selection of one value',
})
const add = schema({ nodeType: 'math.add', label: 'Add', category: 'Math' })

const catalog = [number, add, selectionFilter, getSelection]

test('categories come from the catalog, sorted, behind All', () => {
  assert.deepEqual(browserCategories(catalog), [ALL_CATEGORY, 'Archicad', 'Input', 'Math'])
  // A category the runtime leaves blank is not a tab.
  assert.deepEqual(browserCategories([schema({ nodeType: 'x', label: 'X', category: '' })]), [ALL_CATEGORY])
})

test('score bands rank names above ids and descriptions', () => {
  assert.equal(scoreSchema(getSelection, 'Get Selection'), 0)
  assert.equal(scoreSchema(getSelection, 'get'), 1)
  assert.equal(scoreSchema(getSelection, 'selection'), 2)
  assert.equal(scoreSchema(getSelection, 'archicad.get'), 3)
  assert.equal(scoreSchema(getSelection, 'getselection'), 4)
  // Only the description mentions "currently".
  assert.equal(scoreSchema(getSelection, 'currently'), 5)
  assert.equal(scoreSchema(getSelection, 'gtsl'), 6)
  assert.equal(scoreSchema(getSelection, 'polyline'), undefined)
  // An empty query matches everything equally, so the alphabetical tie-break wins.
  assert.equal(scoreSchema(add, ''), 0)
})

test('ranking puts the node named after the query first', () => {
  assert.deepEqual(
    rankCatalog(catalog, 'selection', ALL_CATEGORY).map((entry) => entry.label),
    ['Selection Filter', 'Get Selection', 'Number'],
  )
})

test('an empty query lists the whole catalog alphabetically', () => {
  assert.deepEqual(
    rankCatalog(catalog, '', ALL_CATEGORY).map((entry) => entry.label),
    ['Add', 'Get Selection', 'Number', 'Selection Filter'],
  )
  assert.deepEqual(rankCatalog(catalog, '   ', ALL_CATEGORY).length, catalog.length)
})

test('a category narrows the search rather than replacing it', () => {
  assert.deepEqual(
    rankCatalog(catalog, 'selection', 'Archicad').map((entry) => entry.label),
    ['Selection Filter', 'Get Selection'],
  )
  assert.deepEqual(rankCatalog(catalog, 'selection', 'Math'), [])
  assert.deepEqual(
    rankCatalog(catalog, '', 'Math').map((entry) => entry.label),
    ['Add'],
  )
})

test('subsequence matching is in order, not a set of characters', () => {
  assert.equal(subsequenceMatch('get selection', 'gtsl'), true)
  assert.equal(subsequenceMatch('get selection', 'lsg'), false)
  assert.equal(subsequenceMatch('get selection', ''), true)
})

test('arrows walk the flat index down and jump a column sideways', () => {
  // 10 entries laid out in columns of 4: indexes 0-3, 4-7, 8-9.
  assert.equal(moveSelection(0, 10, 4, 'ArrowDown'), 1)
  assert.equal(moveSelection(0, 10, 4, 'ArrowUp'), 0)
  assert.equal(moveSelection(1, 10, 4, 'ArrowRight'), 5)
  assert.equal(moveSelection(5, 10, 4, 'ArrowLeft'), 1)
  assert.equal(moveSelection(0, 10, 4, 'ArrowLeft'), 0)
  // The last column is short: right from index 7 clamps to the last entry
  // rather than landing past the end of the list.
  assert.equal(moveSelection(7, 10, 4, 'ArrowRight'), 9)
  assert.equal(moveSelection(9, 10, 4, 'ArrowDown'), 9)
  assert.equal(moveSelection(3, 0, 4, 'ArrowDown'), 0)
})

test('the dialog is clamped inside the viewport with a margin', () => {
  const dialog = { width: 720, height: 460 }
  const viewport = { width: 1200, height: 800 }
  assert.deepEqual(clampDialogPosition({ x: 100, y: 100 }, dialog, viewport), { x: 100, y: 100 })
  assert.deepEqual(clampDialogPosition({ x: 1190, y: 790 }, dialog, viewport), { x: 468, y: 328 })
  assert.deepEqual(clampDialogPosition({ x: -50, y: -50 }, dialog, viewport), { x: 12, y: 12 })
  // Too small to hold the dialog at all: the top-left corner still wins, so the
  // search field stays reachable.
  assert.deepEqual(clampDialogPosition({ x: 40, y: 40 }, dialog, { width: 300, height: 200 }), { x: 12, y: 12 })
})
