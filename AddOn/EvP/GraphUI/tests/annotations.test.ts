import assert from 'node:assert/strict'
import test from 'node:test'
import type { Node } from '@xyflow/svelte'
import {
  annotationFromSelection,
  boundsFromPoints,
  loadAnnotations,
  resolveFrameBounds,
  saveAnnotations,
} from '../src/annotations.ts'
import type { SchemaNodeData } from '../src/types.ts'

function node(id: string, x: number, y: number): Node<SchemaNodeData> {
  return {
    id,
    position: { x, y },
    data: {
      schema: {
        nodeType: 'test',
        label: 'Test',
        category: 'Test',
        description: '',
        executionDomain: 'worker',
        inputs: [],
        outputs: [],
        parameters: [],
      },
      parameters: [],
    },
  }
}

test('rectangle bounds normalize either drag direction', () => {
  assert.deepEqual(boundsFromPoints({ x: 90, y: 70 }, { x: 10, y: 20 }), {
    x: 10,
    y: 20,
    width: 80,
    height: 50,
  })
})

test('Ctrl+G frame records members and follows their positions', () => {
  const frame = annotationFromSelection('frame-1', [node('a', 100, 100), node('b', 400, 250)])
  assert.ok(frame)
  assert.deepEqual(frame.memberNodeIds, ['a', 'b'])
  const moved = resolveFrameBounds(frame, [node('a', 200, 100), node('b', 500, 250)])
  assert.equal(moved.bounds.x, frame.bounds.x + 100)
})

test('editor metadata round-trips and rejects malformed storage', () => {
  const values = new Map<string, string>()
  const storage = {
    getItem: (key: string) => values.get(key) ?? null,
    setItem: (key: string, value: string) => values.set(key, value),
  }
  const annotation = annotationFromSelection('frame-1', [node('a', 0, 0)])
  assert.ok(annotation)
  saveAnnotations(storage, 'default', [annotation])
  assert.deepEqual(loadAnnotations(storage, 'default'), [annotation])
  values.set('tapioca.graph.editor.broken', '{')
  assert.deepEqual(loadAnnotations(storage, 'broken'), [])
})
