import assert from 'node:assert/strict'
import test from 'node:test'

import {
  metadataChanged,
  nativeStepsSince,
  pushUndoStep,
  snapshotMetadata,
  type MetadataSnapshot,
  type UndoStep,
} from '../src/editor.ts'

function snapshot(positions: Record<string, { x: number; y: number }>): MetadataSnapshot {
  return snapshotMetadata(new Map(Object.entries(positions)), new Map())
}

function step(kind: 'native' | 'editor', label: string, coalesceKey = ''): UndoStep {
  const before = snapshot({ a: { x: 0, y: 0 } })
  const after = snapshot({ a: { x: 10, y: 0 } })
  return { kind, label, coalesceKey, before, after }
}

test('a snapshot shares nothing with the maps it was taken from', () => {
  const positions = new Map([['a', { x: 1, y: 2 }]])
  const taken = snapshotMetadata(positions, new Map())
  // The whole point: a step holds where things WERE, so a later drag of the
  // same node must not rewrite the step that preceded it.
  positions.get('a')!.x = 999
  positions.set('b', { x: 0, y: 0 })
  assert.deepEqual(taken.positions.get('a'), { x: 1, y: 2 })
  assert.equal(taken.positions.has('b'), false)
})

test('a drag that ends where it started is not a change', () => {
  const before = snapshot({ a: { x: 5, y: 5 } })
  assert.equal(metadataChanged(before, snapshot({ a: { x: 5, y: 5 } })), false)
  assert.equal(metadataChanged(before, snapshot({ a: { x: 6, y: 5 } })), true)
})

test('key order does not make two identical snapshots differ', () => {
  const one = snapshot({ a: { x: 0, y: 0 }, b: { x: 1, y: 1 } })
  const other = snapshotMetadata(
    new Map([
      ['b', { x: 1, y: 1 }],
      ['a', { x: 0, y: 0 }],
    ]),
    new Map(),
  )
  assert.equal(metadataChanged(one, other), false)
})

test('editor steps sharing a gesture key fold into one', () => {
  let stack = pushUndoStep([], step('editor', 'Move node', 'drag-7'), 20)
  const second = step('editor', 'Move node', 'drag-7')
  stack = pushUndoStep(stack, second, 20)
  assert.equal(stack.length, 1)
  // The earlier `before` survives; the later `after` wins.
  assert.deepEqual(stack[0].before.positions.get('a'), { x: 0, y: 0 })
  assert.equal(stack[0].after, second.after)
})

test('an empty key never folds, and a native step never folds into an editor one', () => {
  let stack = pushUndoStep([], step('editor', 'Move node'), 20)
  stack = pushUndoStep(stack, step('editor', 'Move node'), 20)
  assert.equal(stack.length, 2)
  stack = pushUndoStep(stack, step('native', 'Add node', 'drag-7'), 20)
  assert.equal(stack.length, 3)
})

test('the timeline is trimmed from the old end at the depth in force', () => {
  let stack: UndoStep[] = []
  for (let index = 0; index < 25; index += 1) stack = pushUndoStep(stack, step('editor', `Move ${index}`), 20)
  assert.equal(stack.length, 20)
  // The oldest went, not the newest: undo must reach what just happened.
  assert.equal(stack[0].label, 'Move 5')
  assert.equal(stack[19].label, 'Move 24')
})

test('a lower depth trims the timeline the same way', () => {
  let stack: UndoStep[] = []
  for (let index = 0; index < 12; index += 1) stack = pushUndoStep(stack, step('editor', `Move ${index}`), 20)
  stack = pushUndoStep(stack, step('editor', 'Move 12'), 5)
  assert.equal(stack.length, 5)
  assert.equal(stack[4].label, 'Move 12')
})

test('new runtime steps are counted from the monotonic push counter', () => {
  assert.equal(nativeStepsSince(0, 0), 0)
  assert.equal(nativeStepsSince(4, 5), 1)
  assert.equal(nativeStepsSince(4, 7), 3)
  // Coalesced on the runtime side: nothing new to put on the timeline.
  assert.equal(nativeStepsSince(9, 9), 0)
  // Undo does not lower it, but a graph reopened under a fresh runtime could;
  // going backwards must never mean "negative steps happened".
  assert.equal(nativeStepsSince(9, 2), 0)
})
