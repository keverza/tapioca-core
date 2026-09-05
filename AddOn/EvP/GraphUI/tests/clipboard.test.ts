import assert from 'node:assert/strict'
import test from 'node:test'

import {
  CLIPBOARD_VERSION,
  collectClipboard,
  duplicationPlan,
  rekeyByAssignment,
  parameterGestureKey,
  parseClipboard,
  centreOn,
  centredPasteAnchor,
  clipboardExtent,
  initialUndoDepth,
  nominalNodeSize,
  DEFAULT_UNDO_DEPTH,
  NOMINAL_NODE_SIZE,
  type GraphClipboard,
} from '../src/editor.ts'

/**
 * The rules these cover are the whole of copy/paste. Each one is a decision
 * somebody will later be tempted to "simplify" into the wrong behaviour, which
 * is exactly why they are pinned here rather than left to the UI.
 */

function node(id: string, x: number, y: number, nodeType = 'number') {
  return {
    id,
    type: 'schema',
    position: { x, y },
    data: { schema: { nodeType }, parameters: [{ parameterId: 'value', value: { valueType: 'double', number: 3 } }] },
  } as never
}

function clipboardOf(ids: string[], edges: GraphClipboard['edges'] = []) {
  const nodes = [node('a', 100, 100), node('b', 180, 260), node('c', 400, 100)]
  const positions = new Map([
    ['a', { x: 100, y: 100 }],
    ['b', { x: 180, y: 260 }],
    ['c', { x: 400, y: 100 }],
  ])
  const visuals = new Map([['a', { nickname: 'Width', color: '#7d94ae' }]])
  return collectClipboard(ids, nodes, edges, positions, visuals)
}

let counter = 0
/** What the editor passes: a name local to the transaction, not an id. */
const newAlias = () => `paste-${++counter}`
test.beforeEach(() => {
  counter = 0
})

test('a wire is copied only when BOTH of its ends are', () => {
  const edges = [
    { sourceNode: 'a', sourcePort: 'value', targetNode: 'b', targetPort: 'left' },
    // c is not in the selection, so this one is dropped rather than reconnected
    // to the original: a copy silently wired to the source's upstream is the
    // behaviour that makes duplicate untrustworthy.
    { sourceNode: 'c', sourcePort: 'value', targetNode: 'b', targetPort: 'right' },
  ]
  const clipboard = clipboardOf(['a', 'b'], edges)
  assert.equal(clipboard.nodes.length, 2)
  assert.equal(clipboard.edges.length, 1)
  assert.equal(clipboard.edges[0].sourceNode, 'a')
})

test('ids are remapped, and the originals survive only to rewrite the wires', () => {
  const clipboard = clipboardOf(['a', 'b'], [
    { sourceNode: 'a', sourcePort: 'value', targetNode: 'b', targetPort: 'left' },
  ])
  const plan = duplicationPlan(clipboard, { x: 0, y: 0 }, newAlias)

  const adds = plan.edits.filter((edit) => edit.editKind === 'addNode')
  const connects = plan.edits.filter((edit) => edit.editKind === 'connect')
  assert.equal(adds.length, 2)
  assert.equal(connects.length, 1)

  // ⚠️ NO ADD CARRIES A nodeId AT ALL. The runtime names nodes; an id invented
  // here would be a second authority on identity, and the whole point of the
  // alias is that the browser has stopped having an opinion about it.
  for (const add of adds) {
    assert.equal((add as { nodeId?: string }).nodeId, undefined)
    assert.ok(add.alias !== '' && !['a', 'b', 'c'].includes(add.alias))
  }
  // And the wire names the aliases, not the ids it was copied from.
  assert.equal(connects[0].sourceNode, plan.renames.get('a'))
  assert.equal(connects[0].targetNode, plan.renames.get('b'))
  assert.equal(connects[0].targetPort, 'left')
})

test('nodes are added before the wires that name them', () => {
  const clipboard = clipboardOf(['a', 'b'], [
    { sourceNode: 'a', sourcePort: 'value', targetNode: 'b', targetPort: 'left' },
  ])
  const kinds = duplicationPlan(clipboard, { x: 0, y: 0 }, newAlias).edits.map((edit) => edit.editKind)
  // The runtime applies a batch in order, so a connect naming a node the batch
  // has not added yet would be refused and take the whole transaction with it.
  assert.deepEqual(kinds, ['addNode', 'addNode', 'connect'])
})

test('relative geometry survives; absolute position does not', () => {
  const clipboard = clipboardOf(['a', 'b'])
  const plan = duplicationPlan(clipboard, { x: 500, y: 20 }, newAlias)
  const a = plan.positions.get(plan.renames.get('a')!)!
  const b = plan.positions.get(plan.renames.get('b')!)!

  // The selection's top-left lands exactly on `at`...
  assert.deepEqual(a, { x: 500, y: 20 })
  // ...and the cluster keeps its shape rather than collapsing onto one point.
  assert.equal(b.x - a.x, 80)
  assert.equal(b.y - a.y, 160)
})

test('presentation rides along, keyed to the new id', () => {
  const plan = duplicationPlan(clipboardOf(['a']), { x: 0, y: 0 }, newAlias)
  const copy = plan.renames.get('a')!
  assert.equal(plan.visuals.get(copy)?.nickname, 'Width')
  // A copy that loses its nickname, colour and size is not a copy.
  assert.equal(plan.visuals.get(copy)?.color, '#7d94ae')
})

test('parameters are carried, and omitted rather than sent empty', () => {
  const clipboard = clipboardOf(['a'])
  const withParams = duplicationPlan(clipboard, { x: 0, y: 0 }, newAlias)
  assert.equal(withParams.edits[0].editKind, 'addNode')
  assert.equal((withParams.edits[0] as { parameters?: unknown[] }).parameters?.length, 1)

  clipboard.nodes[0].parameters = []
  const without = duplicationPlan(clipboard, { x: 0, y: 0 }, newAlias)
  // Absent, not []: an empty array claims the node had no parameters rather
  // than that none were copied, and the schema accepts the property missing.
  assert.equal('parameters' in without.edits[0], false)
})

test('pasting twice yields two independent sets', () => {
  const clipboard = clipboardOf(['a', 'b'])
  const first = duplicationPlan(clipboard, { x: 0, y: 0 }, newAlias)
  const second = duplicationPlan(clipboard, { x: 40, y: 40 }, newAlias)
  const firstIds = [...first.renames.values()]
  const secondIds = [...second.renames.values()]
  assert.equal(firstIds.some((id) => secondIds.includes(id)), false)
  // The clipboard is not consumed by a paste.
  assert.equal(clipboard.nodes.length, 2)
})

test('an empty clipboard plans nothing rather than throwing', () => {
  const plan = duplicationPlan({ version: CLIPBOARD_VERSION, nodes: [], edges: [] }, { x: 0, y: 0 }, newAlias)
  assert.deepEqual(plan.edits, [])
  assert.equal(plan.renames.size, 0)
})

test('a clipboard holding anything else is rejected, never half-read', () => {
  // The OS clipboard can hold literally anything; a paste of someone's shopping
  // list is a status message, not a broken editor.
  assert.equal(parseClipboard('not json at all'), undefined)
  assert.equal(parseClipboard('null'), undefined)
  assert.equal(parseClipboard('{"version":99,"nodes":[],"edges":[]}'), undefined)
  assert.equal(parseClipboard('{"version":1,"nodes":"nope","edges":[]}'), undefined)
  // A node without a position would paste at NaN and vanish.
  assert.equal(parseClipboard('{"version":1,"nodes":[{"nodeId":"a","nodeType":"number"}],"edges":[]}'), undefined)
})

test('a well-formed clipboard round-trips', () => {
  const original = clipboardOf(['a', 'b'], [
    { sourceNode: 'a', sourcePort: 'value', targetNode: 'b', targetPort: 'left' },
  ])
  const parsed = parseClipboard(JSON.stringify(original))
  assert.notEqual(parsed, undefined)
  assert.equal(parsed!.nodes.length, 2)
  assert.equal(parsed!.edges.length, 1)
  assert.equal(parsed!.nodes[0].visual?.nickname, 'Width')
})

test('the gesture token is what keeps two drags of one slider apart', () => {
  // Same parameter, same node, different gesture: the runtime coalesces on the
  // whole key, so without the token two separate drags would collapse into one
  // undo step.
  assert.equal(parameterGestureKey('n', 'value', 1), 'setParam:n:value:1')
  assert.notEqual(parameterGestureKey('n', 'value', 1), parameterGestureKey('n', 'value', 2))
})

test('a point places the CENTRE of a node, not its top-left corner', () => {
  // The whole bug: a node asked for at the pointer used to appear down and to
  // the right of it by half its own body.
  const size = nominalNodeSize(undefined)
  const at = centreOn({ x: 500, y: 300 }, size)
  assert.equal(at.x + size.width / 2, 500)
  assert.equal(at.y + size.height / 2, 300)
})

test('a preview node is centred by its own larger body', () => {
  assert.notEqual(nominalNodeSize('preview').width, nominalNodeSize(undefined).width)
  const at = centreOn({ x: 0, y: 0 }, nominalNodeSize('preview'))
  assert.ok(at.x < centreOn({ x: 0, y: 0 }, nominalNodeSize(undefined)).x)
})

test('a pasted cluster is centred as a whole, not by its first node', () => {
  const clipboard = clipboardOf(['a', 'b'])
  const extent = clipboardExtent(clipboard)
  // Corners span 80x160, plus one node body.
  assert.equal(extent.width, 80 + NOMINAL_NODE_SIZE.width)
  assert.equal(extent.height, 160 + NOMINAL_NODE_SIZE.height)

  const at = centredPasteAnchor(clipboard, { x: 1000, y: 500 })
  assert.equal(at.x + extent.width / 2, 1000)
  assert.equal(at.y + extent.height / 2, 500)
})

test('an empty clipboard has no extent and centres harmlessly', () => {
  const empty = { version: 1, nodes: [], edges: [] } as GraphClipboard
  assert.deepEqual(clipboardExtent(empty), { width: 0, height: 0 })
  assert.deepEqual(centredPasteAnchor(empty, { x: 10, y: 20 }), { x: 10, y: 20 })
})

test('the undo depth falls back to 20, and refuses nonsense from storage', () => {
  const store = (value: string | null) => ({ getItem: () => value })
  assert.equal(initialUndoDepth(undefined), DEFAULT_UNDO_DEPTH)
  assert.equal(initialUndoDepth(store(null)), DEFAULT_UNDO_DEPTH)
  assert.equal(initialUndoDepth(store('not a number')), DEFAULT_UNDO_DEPTH)
  assert.equal(initialUndoDepth(store('0')), DEFAULT_UNDO_DEPTH)
  assert.equal(initialUndoDepth(store('99999')), DEFAULT_UNDO_DEPTH)
  assert.equal(initialUndoDepth(store('50')), 50)
})

test('a plan carries aliases, and the runtime says what they became', () => {
  const clipboard = clipboardOf(['a', 'b'])
  const plan = duplicationPlan(clipboard, { x: 100, y: 100 }, newAlias)
  const aliases = [...plan.positions.keys()]
  assert.deepEqual(aliases, [plan.renames.get('a'), plan.renames.get('b')])

  // What the transaction answers with.
  const assigned = [
    { alias: aliases[0], nodeId: 'number-7' },
    { alias: aliases[1], nodeId: 'add-8' },
  ]
  const positions = rekeyByAssignment(plan.positions, assigned)
  assert.deepEqual([...positions.keys()], ['number-7', 'add-8'])
  assert.deepEqual(positions.get('number-7'), plan.positions.get(aliases[0]))

  // The nickname followed its node to the id the runtime chose.
  const visuals = rekeyByAssignment(plan.visuals, assigned)
  assert.equal(visuals.get('number-7')?.nickname, 'Width')
})

test('an alias the runtime did not answer for is dropped, not guessed', () => {
  const plan = duplicationPlan(clipboardOf(['a', 'b']), { x: 0, y: 0 }, newAlias)
  const aliases = [...plan.positions.keys()]
  // Layout kept under a name no node has is how an editor comes to hold
  // positions for nodes that do not exist.
  const positions = rekeyByAssignment(plan.positions, [{ alias: aliases[0], nodeId: 'number-1' }])
  assert.deepEqual([...positions.keys()], ['number-1'])
})

test('rekeying an empty assignment yields nothing rather than the aliases', () => {
  const plan = duplicationPlan(clipboardOf(['a']), { x: 0, y: 0 }, newAlias)
  assert.equal(rekeyByAssignment(plan.positions, []).size, 0)
})
