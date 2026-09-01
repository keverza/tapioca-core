import assert from 'node:assert/strict'
import test from 'node:test'
import type { Node } from '@xyflow/svelte'
import {
  annotationAtPoint,
  annotationFromSelection,
  boundsFromPoints,
  loadAnnotations,
  removeAnnotation,
  renameAnnotation,
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

test('an annotation can be renamed and deleted, which is what makes one temporary', () => {
  const rectangle = {
    id: 'rectangle-1',
    kind: 'rectangle' as const,
    bounds: { x: 10, y: 20, width: 100, height: 60 },
    label: 'Annotation',
    memberNodeIds: [],
  }
  const renamed = renameAnnotation([rectangle], 'rectangle-1', '  Staging area  ')
  assert.equal(renamed[0].label, 'Staging area')
  // An empty label is kept as empty rather than refused: that is how a caption
  // is removed once it has been typed.
  assert.equal(renameAnnotation(renamed, 'rectangle-1', '   ')[0].label, '')
  assert.deepEqual(renameAnnotation([rectangle], 'missing', 'x'), [rectangle])

  assert.deepEqual(removeAnnotation([rectangle], 'rectangle-1'), [])
  assert.deepEqual(removeAnnotation([rectangle], 'missing'), [rectangle])
})

test('hit testing picks the topmost annotation under a point', () => {
  const under = {
    id: 'under',
    kind: 'rectangle' as const,
    bounds: { x: 0, y: 0, width: 200, height: 200 },
    label: 'Under',
    memberNodeIds: [],
  }
  const over = { ...under, id: 'over', bounds: { x: 50, y: 50, width: 50, height: 50 }, label: 'Over' }
  assert.equal(annotationAtPoint([under, over], { x: 60, y: 60 })?.id, 'over')
  assert.equal(annotationAtPoint([under, over], { x: 10, y: 10 })?.id, 'under')
  assert.equal(annotationAtPoint([under, over], { x: 400, y: 10 }), undefined)
  // The edges belong to the rectangle, so a press on the border selects it.
  assert.equal(annotationAtPoint([under], { x: 200, y: 200 })?.id, 'under')
})
