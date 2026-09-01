<script lang="ts">
  /**
   * The node's own viewport, drawing THE GEOMETRY THE GRAPH PRODUCED.
   *
   * ⚠️ IT USED TO DRAW SUBSTITUTE BOXES - one per item, on a spiral, spinning -
   * built from the result's item COUNT and nothing else. A Sphere node showed
   * three green cubes. That is not an abstraction of the result, it is a picture
   * of something the graph never produced, and a user comparing it against their
   * model had no way to tell. Everything here now comes from the value the
   * runtime sent; see nodes/viewer/geometry.ts.
   *
   * ⚠️ AND WHAT COULD NOT BE SENT IS SAID, NOT SUBSTITUTED. A mesh past the
   * bridge's encoding cap arrives as counts; the viewer reports that rather than
   * drawing the part it happens to have as though it were the whole.
   */
  import { T, extend, useThrelte, useTask } from '@threlte/core'
  import { OrbitControls } from 'three/addons/controls/OrbitControls.js'
  import type { DisplayRepresentation } from '../types/display'
  import {
    flatTriangles,
    isEmptyGeometry,
    lineSegments,
    meshEdges,
    viewerBounds,
    type ViewerGeometry,
  } from './geometry'

  // ⚠️ NO DIRECT `three` IMPORT. This package has no @types/three, so importing
  // the library here fails the typecheck - and adding the types to get at
  // BufferGeometry would be paying for a dependency to reproduce what Threlte
  // already exposes declaratively. Buffers are built as plain typed arrays and
  // attached through <T.BufferAttribute>, which also keeps geometry.ts free of a
  // renderer and therefore testable.

  let {
    geometry,
    representation,
    color,
    active,
    fitToken,
    resetToken,
  }: {
    geometry: ViewerGeometry
    representation: DisplayRepresentation
    color: string
    active: boolean
    fitToken: number
    resetToken: number
  } = $props()

  extend({ OrbitControls })
  const { camera, renderer } = useThrelte()
  let controls = $state<OrbitControls>()

  const bounds = $derived(viewerBounds(geometry))
  const centre = $derived(
    bounds === null
      ? ([0, 0, 0] as [number, number, number])
      : ([
          (bounds.min.x + bounds.max.x) / 2,
          (bounds.min.y + bounds.max.y) / 2,
          (bounds.min.z + bounds.max.z) / 2,
        ] as [number, number, number]),
  )
  /**
   * Framed from the geometry's SIZE, not from how many things there are. A single
   * 200 m site outline and a single 50 mm bolt both count as one thing and need
   * very different cameras.
   */
  const extent = $derived(
    bounds === null
      ? 1
      : Math.max(
          bounds.max.x - bounds.min.x,
          bounds.max.y - bounds.min.y,
          bounds.max.z - bounds.min.z,
          0.001,
        ),
  )

  // Z is up in this runtime, as it is in Archicad. Three's default camera is
  // Y-up, so the scene is rotated rather than every coordinate being swapped:
  // swapping coordinates would make the numbers in the viewer disagree with the
  // numbers in the node's own fields.
  const upright: [number, number, number] = [-Math.PI / 2, 0, 0]

  const wireframe = $derived(representation === 'wireframe')
  const ghosted = $derived(representation === 'ghosted')

  function frame(): void {
    if (controls === undefined) return
    const distance = extent * 2.4 + 0.5
    controls.target.set(centre[0], centre[2], -centre[1])
    $camera.position.set(centre[0] + distance * 0.7, centre[2] + distance * 0.55, -centre[1] + distance * 0.7)
    controls.update()
  }

  const array = (values: number[]): Float32Array => Float32Array.from(values)

  $effect(() => {
    // Re-frame whenever the geometry changes shape, not only on demand: a node
    // whose slider just doubled its box should not leave the camera behind.
    void geometry
    frame()
  })

  $effect(() => {
    if (fitToken === 0) return
    frame()
  })

  $effect(() => {
    if (resetToken === 0 || controls === undefined) return
    controls.reset()
    frame()
  })

  useTask(() => {
    controls?.update()
  })
</script>

<T.PerspectiveCamera makeDefault fov={45} />
<T.OrbitControls
  args={[$camera, renderer.domElement]}
  bind:ref={controls}
  enabled={active}
  enableDamping
  dampingFactor={0.12}
  screenSpacePanning
/>
<T.AmbientLight intensity={ghosted ? 1.1 : 0.65} />
<T.DirectionalLight intensity={2.2} position={[3, 6, 4]} />

<T.Group rotation={upright}>
  {#if !isEmptyGeometry(geometry)}
    {#each geometry.meshes as mesh, index (index)}
      {#if wireframe}
        <T.LineSegments>
          <T.BufferGeometry>
            <T.BufferAttribute attach="attributes-position" args={[array(meshEdges(mesh)), 3]} />
          </T.BufferGeometry>
          <T.LineBasicMaterial {color} />
        </T.LineSegments>
      {:else}
        {@const faces = flatTriangles(mesh)}
        <T.Mesh>
          <T.BufferGeometry>
            <T.BufferAttribute attach="attributes-position" args={[array(faces.positions), 3]} />
            <T.BufferAttribute attach="attributes-normal" args={[array(faces.normals), 3]} />
          </T.BufferGeometry>
          <!-- Ghosted is the same solid, translucent and not writing depth, so
               what is inside it stays visible. Double-sided because a translucent
               solid you can see into shows its own back faces. -->
          <T.MeshStandardMaterial
            {color}
            roughness={0.62}
            metalness={0.04}
            transparent={ghosted}
            opacity={ghosted ? 0.28 : 1}
            depthWrite={!ghosted}
            side={ghosted ? 2 : 0}
          />
        </T.Mesh>
        {#if ghosted}
          <!-- Its edges stay solid. A ghost with no edges reads as fog rather
               than as a shape you are looking through. -->
          <T.LineSegments>
            <T.BufferGeometry>
              <T.BufferAttribute attach="attributes-position" args={[array(meshEdges(mesh)), 3]} />
            </T.BufferGeometry>
            <T.LineBasicMaterial {color} transparent opacity={0.75} />
          </T.LineSegments>
        {/if}
      {/if}
    {/each}

    {#each geometry.lines as line, index (index)}
      <T.LineSegments>
        <T.BufferGeometry>
          <T.BufferAttribute attach="attributes-position" args={[array(lineSegments(line)), 3]} />
        </T.BufferGeometry>
        <T.LineBasicMaterial {color} />
      </T.LineSegments>
    {/each}

    {#if geometry.points.length > 0}
      <T.Points>
        <T.BufferGeometry>
          <T.BufferAttribute
            attach="attributes-position"
            args={[array(geometry.points.flatMap((point) => [point.x, point.y, point.z])), 3]}
          />
        </T.BufferGeometry>
        <!-- Screen-sized, because a point has no extent and a world-sized dot
             disappears the moment you zoom out. -->
        <T.PointsMaterial {color} size={5} sizeAttenuation={false} />
      </T.Points>
    {/if}
  {/if}
</T.Group>
