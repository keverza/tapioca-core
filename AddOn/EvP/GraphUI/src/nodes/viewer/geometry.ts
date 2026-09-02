import type { GraphValue } from '../../types'

/**
 * The runtime's values, as things a viewer can draw.
 *
 * ⚠️ THIS EXISTS BECAUSE THE VIEWER USED TO DRAW SUBSTITUTE BOXES. It took the
 * result's ITEM COUNT and spun up that many cubes on a spiral - so a Sphere node
 * showed three green cubes, and a user comparing the node against their model had
 * no way to tell that what they were looking at was never produced by the graph.
 * A viewer that invents its subject is worse than no viewer: it is confidently
 * wrong, and nothing on it says so.
 *
 * So everything here comes from the value the runtime actually sent, and anything
 * that could not be sent is REPORTED rather than substituted - see `truncated`.
 *
 * Pure, and separate from the Threlte scene, because "what does this value draw
 * as" is exactly the question a test can ask and a canvas cannot.
 */

export interface ViewerPoint {
  x: number
  y: number
  z: number
}

export interface ViewerLine {
  /** Flat xyz, one triple per point. */
  positions: number[]
  closed: boolean
}

export interface ViewerMesh {
  /** Flat xyz, one triple per vertex. */
  positions: number[]
  indices: number[]
}

export interface ViewerGeometry {
  points: ViewerPoint[]
  lines: ViewerLine[]
  meshes: ViewerMesh[]

  /**
   * Something in the value could not be drawn from what crossed the bridge - a
   * mesh past the encoding cap, or a list too long to spell out. The viewer says
   * so rather than showing the part it happens to have as if it were the whole.
   */
  truncated: boolean

  /**
   * Values with no geometry at all - a number, a string. Counted, so "I wired a
   * number into Preview" and "my geometry never arrived" are different messages.
   */
  nonGeometric: number
}

/** The bounding box of everything, or null when there is nothing. */
export interface ViewerBounds {
  min: ViewerPoint
  max: ViewerPoint
}

const EMPTY: ViewerGeometry = { points: [], lines: [], meshes: [], truncated: false, nonGeometric: 0 }

function pushTriples(source: number[] | undefined): number[] {
  if (source === undefined) return []
  // Trailing partial triples are dropped rather than padded with zeros: a zero
  // is a coordinate, and padding would put a vertex at the origin that the graph
  // never produced.
  const usable = Math.floor(source.length / 3) * 3
  return source.slice(0, usable)
}

function collect(value: GraphValue | undefined, into: ViewerGeometry, depth: number): void {
  if (value === undefined) return
  // The runtime caps nesting too; this is the client's own guard against a
  // hand-edited or future payload, not a second opinion about the same limit.
  if (depth > 32) {
    into.truncated = true
    return
  }

  switch (value.valueType) {
    case 'point3': {
      const numbers = value.numbers
      if (numbers === undefined || numbers.length < 3) return
      into.points.push({ x: numbers[0], y: numbers[1], z: numbers[2] })
      return
    }

    case 'polyline':
    case 'polygon': {
      const positions = pushTriples(value.numbers)
      if (positions.length < 6) return
      // A polygon closes; a polyline does not. The distinction is the runtime's
      // own - a closed curve whose ends coincide and an open one that happens to
      // return to its start are different results.
      into.lines.push({ positions, closed: value.valueType === 'polygon' })
      return
    }

    case 'mesh': {
      const positions = pushTriples(value.numbers)
      const indices = value.indices ?? []
      if (positions.length === 0 || indices.length < 3) {
        // The runtime sent counts only: over the encoding cap, or a mesh inside a
        // list, where every type is summarised.
        into.truncated = true
        return
      }
      into.meshes.push({ positions, indices })
      if (value.truncated === true) into.truncated = true
      return
    }

    case 'list': {
      if (value.items === undefined) {
        // A list the runtime capped. It said how many; it did not send them.
        if ((value.itemCount ?? 0) > 0) into.truncated = true
        return
      }
      for (const item of value.items) collect(item, into, depth + 1)
      if (value.truncated === true) into.truncated = true
      return
    }

    case 'absent':
      return

    default:
      into.nonGeometric += 1
  }
}

export function viewerGeometryFrom(values: (GraphValue | undefined)[]): ViewerGeometry {
  const geometry: ViewerGeometry = { points: [], lines: [], meshes: [], truncated: false, nonGeometric: 0 }
  for (const value of values) collect(value, geometry, 0)
  return geometry
}

export function isEmptyGeometry(geometry: ViewerGeometry): boolean {
  return geometry.points.length === 0 && geometry.lines.length === 0 && geometry.meshes.length === 0
}

export function emptyGeometry(): ViewerGeometry {
  return { ...EMPTY, points: [], lines: [], meshes: [] }
}

/**
 * What the camera has to frame.
 *
 * ⚠️ FROM THE GEOMETRY, NOT FROM AN ITEM COUNT. The old viewer sized its camera
 * from how MANY things there were, which is unrelated to how big they are: a
 * single 200 m site outline and a single 0.05 m bolt both counted as one.
 */
export function viewerBounds(geometry: ViewerGeometry): ViewerBounds | null {
  let minX = Infinity
  let minY = Infinity
  let minZ = Infinity
  let maxX = -Infinity
  let maxY = -Infinity
  let maxZ = -Infinity

  const grow = (x: number, y: number, z: number): void => {
    if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(z)) return
    minX = Math.min(minX, x)
    minY = Math.min(minY, y)
    minZ = Math.min(minZ, z)
    maxX = Math.max(maxX, x)
    maxY = Math.max(maxY, y)
    maxZ = Math.max(maxZ, z)
  }

  for (const point of geometry.points) grow(point.x, point.y, point.z)
  for (const line of geometry.lines) {
    for (let index = 0; index + 2 < line.positions.length; index += 3) {
      grow(line.positions[index], line.positions[index + 1], line.positions[index + 2])
    }
  }
  for (const mesh of geometry.meshes) {
    for (let index = 0; index + 2 < mesh.positions.length; index += 3) {
      grow(mesh.positions[index], mesh.positions[index + 1], mesh.positions[index + 2])
    }
  }

  if (minX > maxX) return null
  return { min: { x: minX, y: minY, z: minZ }, max: { x: maxX, y: maxY, z: maxZ } }
}

/** Line positions as a drawable segment list, with the closing span when closed. */
export function lineSegments(line: ViewerLine): number[] {
  const points = Math.floor(line.positions.length / 3)
  if (points < 2) return []
  const segments: number[] = []
  const at = (index: number): [number, number, number] => [
    line.positions[index * 3],
    line.positions[index * 3 + 1],
    line.positions[index * 3 + 2],
  ]
  for (let index = 0; index + 1 < points; index += 1) {
    segments.push(...at(index), ...at(index + 1))
  }
  if (line.closed) segments.push(...at(points - 1), ...at(0))
  return segments
}

/**
 * The edges of a triangle list, each once.
 *
 * ⚠️ DEDUPLICATED, because every interior edge of a closed mesh belongs to two
 * triangles: drawn twice, a wireframe is twice the geometry and visibly heavier
 * on the shared edges than on the boundary ones, which reads as the mesh having
 * a seam there.
 */
export function meshEdges(mesh: ViewerMesh): number[] {
  const seen = new Set<string>()
  const segments: number[] = []
  const at = (index: number): [number, number, number] => [
    mesh.positions[index * 3],
    mesh.positions[index * 3 + 1],
    mesh.positions[index * 3 + 2],
  ]
  for (let index = 0; index + 2 < mesh.indices.length; index += 3) {
    const corners = [mesh.indices[index], mesh.indices[index + 1], mesh.indices[index + 2]]
    for (let edge = 0; edge < 3; edge += 1) {
      const a = corners[edge]
      const b = corners[(edge + 1) % 3]
      const key = a < b ? `${a}:${b}` : `${b}:${a}`
      if (seen.has(key)) continue
      seen.add(key)
      if (a * 3 + 2 >= mesh.positions.length || b * 3 + 2 >= mesh.positions.length) continue
      segments.push(...at(a), ...at(b))
    }
  }
  return segments
}

/**
 * A mesh as loose triangles with a face normal per vertex.
 *
 * ⚠️ DE-INDEXED ON PURPOSE, for flat shading. A shared vertex can only carry one
 * normal, so an indexed box would average three face normals at every corner and
 * render as a rounded lump with no edges - the same failure the native builder
 * gives 24 vertices to avoid. Three vertices per triangle costs three times the
 * buffer and gives every face its own normal, which is what a modelling preview
 * should show.
 *
 * Doing it here rather than calling three's computeVertexNormals also keeps this
 * module - and the scene - free of a direct three import, so the geometry stays
 * testable without a renderer.
 */
export function flatTriangles(mesh: ViewerMesh): { positions: number[]; normals: number[] } {
  const positions: number[] = []
  const normals: number[] = []
  const vertexCount = Math.floor(mesh.positions.length / 3)

  for (let index = 0; index + 2 < mesh.indices.length; index += 3) {
    const corners = [mesh.indices[index], mesh.indices[index + 1], mesh.indices[index + 2]]
    if (corners.some((corner) => corner < 0 || corner >= vertexCount)) continue

    const [a, b, c] = corners.map((corner) => [
      mesh.positions[corner * 3],
      mesh.positions[corner * 3 + 1],
      mesh.positions[corner * 3 + 2],
    ])
    const u = [b[0] - a[0], b[1] - a[1], b[2] - a[2]]
    const v = [c[0] - a[0], c[1] - a[1], c[2] - a[2]]
    let nx = u[1] * v[2] - u[2] * v[1]
    let ny = u[2] * v[0] - u[0] * v[2]
    let nz = u[0] * v[1] - u[1] * v[0]
    const length = Math.sqrt(nx * nx + ny * ny + nz * nz)
    if (length > 0) {
      nx /= length
      ny /= length
      nz /= length
    } else {
      // A degenerate triangle has no normal. Kept with an up normal rather than
      // dropped, so the vertex count still matches the source and a zero normal
      // never reaches the shader, which would shade it black.
      nx = 0
      ny = 0
      nz = 1
    }

    for (const corner of [a, b, c]) {
      positions.push(corner[0], corner[1], corner[2])
      normals.push(nx, ny, nz)
    }
  }

  return { positions, normals }
}

/**
 * A curve's length along itself, closing span included when it is closed.
 *
 * ⚠️ MEASURED FROM THE POINTS THAT ARRIVED, which for an arc or a circle means
 * the length of its TESSELLATION, not of the ideal curve. The runtime sends a
 * polyline because that is what it built; reporting an analytic length here
 * would be reporting a number the graph never produced.
 */
export function curveLength(line: ViewerLine): number {
  const count = Math.floor(line.positions.length / 3)
  if (count < 2) return 0
  let total = 0
  const span = (from: number, to: number): number => {
    const dx = line.positions[to * 3] - line.positions[from * 3]
    const dy = line.positions[to * 3 + 1] - line.positions[from * 3 + 1]
    const dz = line.positions[to * 3 + 2] - line.positions[from * 3 + 2]
    return Math.sqrt(dx * dx + dy * dy + dz * dz)
  }
  for (let i = 1; i < count; i += 1) total += span(i - 1, i)
  if (line.closed) total += span(count - 1, 0)
  return total
}

/** What a value is MADE OF, once it has been read as geometry. */
export interface GeometryTotals {
  points: number
  curves: number
  meshes: number
  /** Every vertex of every part, curve points included. */
  vertices: number
  triangles: number
  /** Summed curve length. Zero when there are no curves. */
  length: number
}

export function geometryTotals(geometry: ViewerGeometry): GeometryTotals {
  let vertices = geometry.points.length
  let triangles = 0
  let length = 0
  for (const line of geometry.lines) {
    vertices += Math.floor(line.positions.length / 3)
    length += curveLength(line)
  }
  for (const mesh of geometry.meshes) {
    vertices += Math.floor(mesh.positions.length / 3)
    triangles += Math.floor(mesh.indices.length / 3)
  }
  return {
    points: geometry.points.length,
    curves: geometry.lines.length,
    meshes: geometry.meshes.length,
    vertices,
    triangles,
    length,
  }
}
