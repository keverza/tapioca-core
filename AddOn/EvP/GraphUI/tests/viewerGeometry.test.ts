import assert from 'node:assert/strict'
import test from 'node:test'
import {
  flatTriangles,
  isEmptyGeometry,
  lineSegments,
  meshEdges,
  viewerBounds,
  viewerGeometryFrom,
} from '../src/nodes/viewer/geometry.ts'
import type { GraphValue } from '../src/types.ts'

/**
 * ⚠️ THE FAILURE THIS FILE EXISTS FOR: the node viewport used to draw substitute
 * boxes - one per item, from the result's COUNT - so a Sphere showed three green
 * cubes and nothing on the node said the picture was invented. Every test here is
 * about the viewer drawing what the graph produced, or saying it cannot.
 */

test('a value draws as what it is, and nothing draws what was not sent', () => {
  const values: GraphValue[] = [
    { valueType: 'point3', numbers: [1, 2, 3] },
    { valueType: 'polyline', numbers: [0, 0, 0, 1, 0, 0, 1, 1, 0] },
    { valueType: 'polygon', numbers: [0, 0, 0, 2, 0, 0, 2, 2, 0] },
    { valueType: 'mesh', numbers: [0, 0, 0, 1, 0, 0, 0, 1, 0], indices: [0, 1, 2] },
  ]
  const geometry = viewerGeometryFrom(values)

  assert.equal(geometry.points.length, 1)
  assert.deepEqual(geometry.points[0], { x: 1, y: 2, z: 3 })
  assert.equal(geometry.lines.length, 2)
  // A polygon closes and a polyline does not: the runtime's own distinction, and
  // a closed curve whose ends coincide is not the same result as an open one.
  assert.equal(geometry.lines[0].closed, false)
  assert.equal(geometry.lines[1].closed, true)
  assert.equal(geometry.meshes.length, 1)
  assert.equal(geometry.truncated, false)
  assert.equal(isEmptyGeometry(geometry), false)
})

test('a mesh the bridge could not send is reported, never substituted', () => {
  // Over the encoding cap, or inside a list: the runtime sends counts only. The
  // old viewer turned that count into cubes; this one has to say so.
  const capped: GraphValue = { valueType: 'mesh', itemCount: 900000, truncated: true }
  const geometry = viewerGeometryFrom([capped])

  assert.equal(geometry.meshes.length, 0)
  assert.equal(geometry.truncated, true)
  assert.equal(isEmptyGeometry(geometry), true)
})

test('a list the runtime capped counts as truncated rather than as empty', () => {
  const geometry = viewerGeometryFrom([{ valueType: 'list', itemCount: 4000, truncated: true }])
  assert.equal(geometry.truncated, true)

  // ... and an empty list really is empty, not truncated.
  const nothing = viewerGeometryFrom([{ valueType: 'list', itemCount: 0, items: [] }])
  assert.equal(nothing.truncated, false)
})

test('values with no geometry are counted, not silently dropped', () => {
  // "I wired a number into Preview" and "my geometry never arrived" are the same
  // empty viewport and different problems.
  const geometry = viewerGeometryFrom([
    { valueType: 'double', number: 4 },
    { valueType: 'string', text: 'hello' },
    { valueType: 'absent' },
  ])
  assert.equal(geometry.nonGeometric, 2)
  assert.equal(isEmptyGeometry(geometry), true)
})

test('a partial coordinate triple is dropped rather than padded to the origin', () => {
  // A zero IS a coordinate; padding would put a vertex at the origin the graph
  // never produced.
  const geometry = viewerGeometryFrom([{ valueType: 'polyline', numbers: [0, 0, 0, 1, 1, 1, 2, 2] }])
  assert.equal(geometry.lines.length, 1)
  assert.equal(geometry.lines[0].positions.length, 6)
})

test('the camera frames the geometry SIZE, not the item count', () => {
  // A 200 m site outline and a 50 mm bolt are both one thing and need very
  // different cameras.
  const small = viewerBounds(viewerGeometryFrom([{ valueType: 'point3', numbers: [0, 0, 0] }]))
  const large = viewerBounds(
    viewerGeometryFrom([{ valueType: 'polyline', numbers: [0, 0, 0, 200, 0, 0] }]),
  )
  assert.deepEqual(small, { min: { x: 0, y: 0, z: 0 }, max: { x: 0, y: 0, z: 0 } })
  assert.equal(large?.max.x, 200)
  assert.equal(viewerBounds(viewerGeometryFrom([])), null)
})

test('a closed line draws its closing span and an open one does not', () => {
  const open = lineSegments({ positions: [0, 0, 0, 1, 0, 0, 1, 1, 0], closed: false })
  const closed = lineSegments({ positions: [0, 0, 0, 1, 0, 0, 1, 1, 0], closed: true })
  assert.equal(open.length, 2 * 6)
  assert.equal(closed.length, 3 * 6)
  // The closing span runs from the last point back to the first.
  assert.deepEqual(closed.slice(12), [1, 1, 0, 0, 0, 0])
})

test('a wireframe draws each shared edge once', () => {
  // Every interior edge of a closed mesh belongs to two triangles. Drawn twice it
  // is twice the geometry and visibly heavier there, which reads as a seam.
  const quad = {
    positions: [0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0],
    indices: [0, 1, 2, 0, 2, 3],
  }
  // Five edges, not six: the diagonal 0-2 is shared.
  assert.equal(meshEdges(quad).length, 5 * 6)
})

test('a mesh is de-indexed so every face keeps its own normal', () => {
  // A shared vertex can only carry one normal, so an indexed box would average
  // three face normals at every corner and render as a rounded lump.
  const wedge = {
    positions: [0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1],
    indices: [0, 1, 2, 0, 1, 3],
  }
  const faces = flatTriangles(wedge)
  assert.equal(faces.positions.length, 2 * 3 * 3)
  assert.equal(faces.normals.length, faces.positions.length)

  // The first triangle lies in XY, so its normal is +Z ...
  assert.deepEqual(faces.normals.slice(0, 3), [0, 0, 1])
  // ... and the second lies in XZ, so it is not.
  assert.notDeepEqual(faces.normals.slice(9, 12), [0, 0, 1])
})

test('an index outside the vertex array is skipped rather than read past the end', () => {
  const broken = { positions: [0, 0, 0, 1, 0, 0, 0, 1, 0], indices: [0, 1, 2, 0, 1, 99] }
  const faces = flatTriangles(broken)
  assert.equal(faces.positions.length, 3 * 3)
  assert.equal(meshEdges(broken).length, 3 * 6)
})
