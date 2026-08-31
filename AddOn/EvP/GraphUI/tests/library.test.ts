import assert from 'node:assert/strict'
import test from 'node:test'
import type { XYPosition } from '@xyflow/svelte'
import { applyLayoutToPositions, isValidGraphName } from '../src/editor.ts'

test('graph names follow the runtime rule, so a bad one is refused before a round trip', () => {
  for (const good of ['daylight', 'Massing_v2', 'site.analysis', 'a-b-c']) {
    assert.equal(isValidGraphName(good), true, `refused ${good}`)
  }
  // The path-traversal shapes in particular: the runtime rejects these, and the
  // dialog must not offer a Save button that is going to fail.
  for (const bad of ['', '..', '../escape', 'with/slash', 'back\\slash', '.hidden', 'a b', 'sm√∂rg√•s']) {
    assert.equal(isValidGraphName(bad), false, `accepted ${bad}`)
  }
})

test('a saved layout restores node positions', () => {
  const positions = new Map<string, XYPosition>()
  applyLayoutToPositions(
    [
      { nodeId: 'a', fields: [{ key: 'x', value: '120' }, { key: 'y', value: '-40' }] },
      { nodeId: 'b', fields: [{ key: 'y', value: '10' }, { key: 'x', value: '0' }] },
    ],
    positions,
  )
  assert.deepEqual(positions.get('a'), { x: 120, y: -40 })
  assert.deepEqual(positions.get('b'), { x: 0, y: 10 })
})

test('an unusable layout record falls back to automatic placement rather than the origin', () => {
  const positions = new Map<string, XYPosition>()
  applyLayoutToPositions(
    [
      { nodeId: 'missing-y', fields: [{ key: 'x', value: '10' }] },
      { nodeId: 'not-a-number', fields: [{ key: 'x', value: 'left' }, { key: 'y', value: '2' }] },
      // A field written by a client this build does not know about is carried
      // by the runtime and ignored here, not treated as a failure.
      { nodeId: 'extra', fields: [{ key: 'x', value: '5' }, { key: 'y', value: '6' }, { key: 'colour', value: 'red' }] },
    ],
    positions,
  )
  assert.equal(positions.has('missing-y'), false)
  assert.equal(positions.has('not-a-number'), false)
  assert.deepEqual(positions.get('extra'), { x: 5, y: 6 })
})
