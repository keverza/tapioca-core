<script lang="ts">
  import { T, extend, useThrelte, useTask } from '@threlte/core'
  import { OrbitControls } from 'three/addons/controls/OrbitControls.js'
  import type { NodeResultRecord } from '../../types'

  let {
    itemCount,
    status,
    active,
    fitToken,
    resetToken,
  }: {
    itemCount: number
    status: NodeResultRecord['status']
    active: boolean
    fitToken: number
    resetToken: number
  } = $props()

  extend({ OrbitControls })
  const { camera, renderer } = useThrelte()
  let controls = $state<OrbitControls>()
  const assets = Array.from({ length: 24 }, (_, index) => {
    const angle = index * 2.399963
    const radius = 0.45 + Math.sqrt(index) * 0.52
    return {
      position: [Math.cos(angle) * radius, Math.sin(angle) * radius * 0.62, (index % 5) * 0.22 - 0.44] as const,
      rotation: [angle * 0.31, angle * 0.17, angle * 0.23] as const,
      speed: 0.35 + (index % 7) * 0.08,
    }
  })

  let elapsed = $state(0)
  const visibleAssets = $derived(assets.slice(0, Math.min(24, Math.max(1, itemCount))))
  const color = $derived(
    status === 'success'
      ? '#75c695'
      : status === 'error' || status === 'blocked'
        ? '#e36d5b'
        : '#ffb000',
  )

  $effect(() => {
    $camera.position.set(0, 1.3, 8.5)
  })

  $effect(() => {
    if (fitToken === 0 || controls === undefined) return
    const distance = Math.max(5, 4 + Math.sqrt(Math.max(1, itemCount)) * 0.65)
    controls.target.set(0, 0, 0)
    $camera.position.set(0, distance * 0.16, distance)
    controls.update()
  })

  $effect(() => {
    if (resetToken === 0 || controls === undefined) return
    controls.reset()
  })

  useTask((delta) => {
    elapsed += delta
    controls?.update()
  })
</script>

<T.PerspectiveCamera
  makeDefault
  on:create={({ ref }) => {
    ref.lookAt(0, 0, 0)
  }}
/>
<T.OrbitControls args={[$camera, renderer.domElement]} bind:ref={controls} enabled={active} enableDamping dampingFactor={0.12} screenSpacePanning />
<T.AmbientLight intensity={0.7} />
<T.DirectionalLight intensity={2.5} position={[3, 5, 8]} />

{#each visibleAssets as asset, index}
  <T.Mesh
    position={asset.position}
    rotation={[
      asset.rotation[0] + elapsed * asset.speed,
      asset.rotation[1] + elapsed * asset.speed * 0.7,
      asset.rotation[2],
    ]}
  >
    {#if index % 3 === 0}
      <T.TetrahedronGeometry args={[0.35, 0]} />
    {:else}
      <T.BoxGeometry args={[0.5, 0.5, 0.5]} />
    {/if}
    <T.MeshLambertMaterial {color} toneMapped={false} />
  </T.Mesh>
{/each}
