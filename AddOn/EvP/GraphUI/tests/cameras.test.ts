import assert from 'node:assert/strict'
import test from 'node:test'
import { camerasOf, formatPosition, formatSun, parseCamera } from '../src/nodes/archicad/cameras.ts'
import type { GraphParameter } from '../src/types.ts'

/**
 * ⚠️ WHAT THESE TESTS PROTECT. Two things, and both fail silently.
 *
 * The first is INDEX PARITY. The position a row is drawn at is the index the
 * Remove and Restore actions send to the runtime, so a browser that quietly
 * dropped an unreadable row would make every button below it act on a different
 * camera than the one the user clicked - a delete that removes the wrong thing
 * and reports success.
 *
 * The second is that a ONE-CAMERA LIST IS NOT A LIST. The runtime's tree layer
 * projects a single-item branch back as that item, so a node holding exactly one
 * camera sends a bare string. Reading only the `items` array would draw an empty
 * list beside a node whose count says one.
 */

function camera(eyeX: number): string {
  return JSON.stringify({
    valid: true,
    source: 'perspective',
    orthographic: false,
    viewMoving: false,
    eyeX,
    eyeY: 2,
    eyeZ: 3,
    targetX: 4,
    targetY: 5,
    targetZ: 6,
    viewConeDegreesHorizontal: 60,
    hasSun: true,
    sunAzimuthDegrees: -80.25,
    sunAltitudeDegrees: 35.5,
    sunBearingDegrees: 193.75,
    sunSource: 'date',
  })
}

function parameters(...texts: string[]): GraphParameter[] {
  return [{ parameterId: 'cameras', value: { valueType: 'list', items: texts.map((text) => ({ valueType: 'string', text })) } }]
}

test('a stored camera round trips every field the renderer needs', () => {
  const parsed = parseCamera(camera(10))
  assert.ok(parsed)
  assert.equal(parsed.valid, true)
  assert.equal(parsed.source, 'perspective')
  assert.deepEqual(parsed.eye, [10, 2, 3])
  assert.deepEqual(parsed.target, [4, 5, 6])
  assert.equal(parsed.viewConeDegreesHorizontal, 60)
})

test('the sun is read off the row and shown as a compass bearing', () => {
  const parsed = parseCamera(camera(10))
  assert.ok(parsed)
  assert.equal(parsed.hasSun, true)
  // The MODEL angle survives as-is, negative and all: it is a mathematical
  // angle, not a bearing, and it is what the renderer takes.
  assert.equal(parsed.sunAzimuthDegrees, -80.25)
  // The reader gets the compass one. Showing -80 here is how a correct sun gets
  // reported as a bug.
  assert.equal(formatSun(parsed), '194° bearing, 36° up (date)')
})

test('a camera with no sun shows none rather than a sun on the horizon', () => {
  // A row saved before sun capture existed has no sun fields at all. Zero is a
  // real direction - due east on the horizon - so the flag has to be read.
  const legacy = parseCamera(
    JSON.stringify({ valid: true, source: 'perspective', eyeX: 1, eyeY: 2, eyeZ: 3, targetX: 4, targetY: 5, targetZ: 6, viewConeDegreesHorizontal: 60 }),
  )
  assert.ok(legacy)
  assert.equal(legacy.hasSun, false)
  assert.equal(formatSun(legacy), undefined)
})

test('cameras are read in capture order', () => {
  const cameras = camerasOf(parameters(camera(1), camera(2), camera(3)))
  assert.equal(cameras.length, 3)
  assert.deepEqual(
    cameras.map((entry) => entry?.eye[0]),
    [1, 2, 3],
  )
})

test('one camera arrives as an item rather than a list of one', () => {
  // The runtime's own decoder accepts both for exactly this reason; see
  // CamerasFromValue. Reading only `items` here would draw nothing.
  const cameras = camerasOf([{ parameterId: 'cameras', value: { valueType: 'string', text: camera(7) } }])
  assert.equal(cameras.length, 1)
  assert.equal(cameras[0]?.eye[0], 7)
})

test('an unreadable row keeps its place so later indices stay true', () => {
  const cameras = camerasOf(parameters(camera(1), 'not json at all', camera(3)))
  assert.equal(cameras.length, 3)
  assert.equal(cameras[1], undefined)
  // The third camera is still at index 2, which is what Remove and Restore send.
  assert.equal(cameras[2]?.eye[0], 3)
})

test('a camera missing its coordinates is unreadable rather than sitting at the origin', () => {
  // A zeroed camera would look like a real row and point the 3D window nowhere
  // useful the moment somebody pressed Restore on it.
  assert.equal(parseCamera(JSON.stringify({ valid: true, source: 'perspective' })), undefined)
  assert.equal(parseCamera('[]'), undefined)
  assert.equal(parseCamera(''), undefined)
})

test('an absent parameter is an empty list, not a failure', () => {
  // A node that has been dropped and not yet used has no parameter at all.
  assert.deepEqual(camerasOf([]), [])
})

test('positions read as metres at the precision a person reads them', () => {
  assert.equal(formatPosition([1.234, -5.678, 0]), '1.2, -5.7, 0.0')
})
