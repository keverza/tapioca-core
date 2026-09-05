import assert from 'node:assert/strict'
import test from 'node:test'
import {
  ELEMENT_SETTING_NODE_TYPE,
  dockFromFields,
  dockToFields,
  dockedRows,
  nextDockOrder,
  promotionPlan,
  rekeyByAssignment,
  withDockedRows,
  applyLayoutToPositions,
  layoutFromPositions,
  dockedNodePosition,
  DOCK_ROW_HEIGHT,
  type DockState,
} from '../src/editor.ts'

/**
 * Promotion: a property browser row becoming a node docked under its host.
 *
 * ⚠️ WHAT THESE ARE FOR. A promoted row and an explicit projection node are the
 * SAME node drawn two ways, and the only thing separating them is a layout
 * field. These tests pin the two halves of that: the transaction is atomic (a
 * promotion cannot half-apply into a node wired to nothing), and a dock that
 * points at something absent degrades to a visible box rather than to a node
 * nobody can see.
 */

const alias = (nodeType: string) => `${nodeType}#1`

test('a promotion adds and wires in one transaction', () => {
  // ⚠️ BOTH EDITS OR NEITHER. Adding the node in one call and connecting it in
  // another can leave a setting node wired to nothing if the second never lands
  // - a node the user did not ask for and cannot explain.
  const plan = promotionPlan('walls', 'elements', 'height', 'wall', 0, alias)

  assert.equal(plan.edits.length, 2)
  const [add, connect] = plan.edits
  assert.equal(add.editKind, 'addNode')
  assert.equal(connect.editKind, 'connect')

  assert.equal(add.editKind === 'addNode' && add.nodeType, ELEMENT_SETTING_NODE_TYPE)
  // The wire names the node by its ALIAS, because its real id does not exist
  // yet - the runtime assigns it and answers with it.
  assert.equal(connect.editKind === 'connect' && connect.targetNode, plan.alias)
  assert.equal(connect.editKind === 'connect' && connect.sourceNode, 'walls')
  assert.equal(connect.editKind === 'connect' && connect.sourcePort, 'elements')
})

test('the promotion stores which setting and which type it was made against', () => {
  // elementType is stored rather than inferred: rewiring the host to another
  // type must be visible instead of silently answering anyway.
  const plan = promotionPlan('walls', 'elements', 'height', 'wall', 0, alias)
  const add = plan.edits[0]
  assert.equal(add.editKind, 'addNode')
  if (add.editKind !== 'addNode') return

  const byId = new Map(add.parameters?.map((p) => [p.parameterId, p.value?.text]))
  assert.equal(byId.get('setting'), 'height')
  assert.equal(byId.get('elementType'), 'wall')
})

test('the dock is keyed by alias and rekeys onto the id the runtime chose', () => {
  const plan = promotionPlan('walls', 'elements', 'height', 'wall', 2, alias)
  assert.deepEqual(plan.docks.get(plan.alias), { dockedTo: 'walls', dockOrder: 2 })

  const docks = rekeyByAssignment(plan.docks, [{ alias: plan.alias, nodeId: 'node-7' }])
  assert.deepEqual(docks.get('node-7'), { dockedTo: 'walls', dockOrder: 2 })
  assert.equal(docks.has(plan.alias), false)
})

test('dock fields round-trip through layout', () => {
  const dock: DockState = { dockedTo: 'walls', dockOrder: 3 }
  assert.deepEqual(dockFromFields(dockToFields(dock)), dock)
})

test('a node with no dockedTo is an ordinary box', () => {
  assert.equal(dockFromFields([{ key: 'x', value: '10' }]), undefined)
  assert.equal(dockFromFields([{ key: 'dockedTo', value: '' }]), undefined)
})

test('a dock with no usable order goes last rather than being discarded', () => {
  // ⚠️ THE AUTHOR STILL MEANT IT TO BE A ROW. Dropping the dock over a bad
  // number scatters a node's promoted rows across the canvas, which reads as
  // data loss; a row in the wrong place does not.
  const dock = dockFromFields([
    { key: 'dockedTo', value: 'walls' },
    { key: 'dockOrder', value: 'not a number' },
  ])
  assert.equal(dock?.dockedTo, 'walls')
  assert.equal(dock?.dockOrder, Number.MAX_SAFE_INTEGER)
})

test('rows group under their host in dock order', () => {
  const docks = new Map<string, DockState>([
    ['b', { dockedTo: 'walls', dockOrder: 1 }],
    ['a', { dockedTo: 'walls', dockOrder: 0 }],
    ['c', { dockedTo: 'slabs', dockOrder: 0 }],
  ])
  const { byHost, orphans } = dockedRows(docks, ['walls', 'slabs', 'a', 'b', 'c'])
  assert.deepEqual(byHost.get('walls'), ['a', 'b'])
  assert.deepEqual(byHost.get('slabs'), ['c'])
  assert.deepEqual(orphans, [])
})

test('two rows sharing an order keep a stable sequence between repaints', () => {
  const docks = new Map<string, DockState>([
    ['z', { dockedTo: 'walls', dockOrder: 0 }],
    ['a', { dockedTo: 'walls', dockOrder: 0 }],
  ])
  assert.deepEqual(dockedRows(docks, ['walls', 'a', 'z']).byHost.get('walls'), ['a', 'z'])
})

test('a row whose host is gone becomes a visible box, never a hidden node', () => {
  // ⚠️ THE CASE THAT WOULD BE WORST TO GET WRONG. A dock pointing at a deleted
  // node must not make its row disappear: it would still be in the document and
  // still evaluating, and the user could not see, select or delete it.
  const docks = new Map<string, DockState>([['a', { dockedTo: 'deleted', dockOrder: 0 }]])
  const { byHost, orphans } = dockedRows(docks, ['a'])
  assert.equal(byHost.size, 0)
  assert.deepEqual(orphans, ['a'])
})

test('a row docked to itself is an orphan rather than an infinite nesting', () => {
  const docks = new Map<string, DockState>([['a', { dockedTo: 'a', dockOrder: 0 }]])
  assert.deepEqual(dockedRows(docks, ['a']).orphans, ['a'])
})

test('promoting again appends rather than renumbering the existing rows', () => {
  // Renumbering would move the rows a user has already arranged, every time
  // they promote one more property.
  const docks = new Map<string, DockState>([
    ['a', { dockedTo: 'walls', dockOrder: 0 }],
    ['b', { dockedTo: 'walls', dockOrder: 4 }],
    ['c', { dockedTo: 'slabs', dockOrder: 9 }],
  ])
  assert.equal(nextDockOrder(docks, 'walls'), 5)
  // A host with no rows yet starts at zero rather than at the global maximum.
  assert.equal(nextDockOrder(docks, 'beams'), 0)
})

test('a docked row saves its dock and not a position', () => {
  // ⚠️ TWO ANSWERS TO ONE QUESTION IS THE BUG. A row's coordinates are derived
  // from its host and its order; persisting them too would let the file disagree
  // with the derivation the moment the host moved.
  const positions = new Map([
    ['walls', { x: 100, y: 40 }],
    ['row', { x: 999, y: 999 }],
  ])
  const docks = new Map<string, DockState>([['row', { dockedTo: 'walls', dockOrder: 1 }]])

  const layout = layoutFromPositions(positions, ['walls', 'row'], docks)
  const forRow = layout.find((record) => record.nodeId === 'row')
  assert.deepEqual(
    forRow?.fields.map((f) => f.key).sort(),
    ['dockOrder', 'dockedTo'],
  )
  const forHost = layout.find((record) => record.nodeId === 'walls')
  assert.deepEqual(
    forHost?.fields.map((f) => f.key).sort(),
    ['x', 'y'],
  )
})

test('layout round-trips docks and positions to the right stores', () => {
  const positions = new Map([['walls', { x: 100, y: 40 }]])
  const docks = new Map<string, DockState>([['row', { dockedTo: 'walls', dockOrder: 2 }]])
  const layout = layoutFromPositions(positions, ['walls', 'row'], docks)

  const backPositions = new Map()
  const backDocks = new Map<string, DockState>()
  applyLayoutToPositions(layout, backPositions, backDocks)

  assert.deepEqual(backPositions.get('walls'), { x: 100, y: 40 })
  assert.deepEqual(backDocks.get('row'), { dockedTo: 'walls', dockOrder: 2 })
  // The row got no position, so it falls to automatic placement rather than to
  // the origin, if a caller ever draws it as a box.
  assert.equal(backPositions.has('row'), false)
})

test('a caller that ignores docks still loads the rest of the layout', () => {
  // applyLayoutToPositions is called in one place without a dock store; a
  // missing argument must not throw or lose the ordinary nodes.
  const layout = layoutFromPositions(
    new Map([['walls', { x: 5, y: 6 }]]),
    ['walls', 'row'],
    new Map<string, DockState>([['row', { dockedTo: 'walls', dockOrder: 0 }]]),
  )
  const positions = new Map()
  applyLayoutToPositions(layout, positions)
  assert.deepEqual(positions.get('walls'), { x: 5, y: 6 })
})

test('rows stack under their host by order', () => {
  assert.deepEqual(dockedNodePosition(60, 0), { x: 0, y: 60 })
  assert.deepEqual(dockedNodePosition(60, 2), { x: 0, y: 60 + 2 * DOCK_ROW_HEIGHT })
  // A host whose height has not been measured yet must not push rows above it.
  assert.deepEqual(dockedNodePosition(-10, 0), { x: 0, y: 0 })
})

test('deleting a host takes its docked rows with it', () => {
  // ⚠️ THE COST OF DRAWING A NODE AS A ROW, PAID. A row left behind is a node
  // the user cannot see, cannot reach, and did not know survived - still
  // evaluating and still reading the model.
  const docks = new Map<string, DockState>([
    ['h1', { dockedTo: 'walls', dockOrder: 0 }],
    ['h2', { dockedTo: 'walls', dockOrder: 1 }],
    ['other', { dockedTo: 'slabs', dockOrder: 0 }],
  ])
  assert.deepEqual(withDockedRows(['walls'], docks).sort(), ['h1', 'h2', 'walls'])
})

test('deleting a row alone leaves its host and its siblings', () => {
  const docks = new Map<string, DockState>([
    ['h1', { dockedTo: 'walls', dockOrder: 0 }],
    ['h2', { dockedTo: 'walls', dockOrder: 1 }],
  ])
  assert.deepEqual(withDockedRows(['h1'], docks), ['h1'])
})

test('deleting a node that hosts nothing is unchanged', () => {
  assert.deepEqual(withDockedRows(['lonely'], new Map()), ['lonely'])
})
