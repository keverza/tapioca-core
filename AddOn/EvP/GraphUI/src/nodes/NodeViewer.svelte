<script lang="ts">
  import { Canvas } from '@threlte/core'
  import type { GraphValue } from '../types'
  import type { DisplayRepresentation } from './types/display'
  import ViewerScene from './viewer/ViewerScene.svelte'
  import ViewerToolbar from './viewer/ViewerToolbar.svelte'
  import { isEmptyGeometry, viewerGeometryFrom } from './viewer/geometry'

  let {
    values,
    representation,
    color,
    active,
    onactive,
  }: {
    values: GraphValue[]
    representation: DisplayRepresentation
    color: string
    active: boolean
    onactive: () => void
  } = $props()
  let fitToken = $state(0)
  let resetToken = $state(0)

  const geometry = $derived(viewerGeometryFrom(values))
  const empty = $derived(isEmptyGeometry(geometry))

  /**
   * ⚠️ THE CAPTION SAYS WHAT IS AND IS NOT BEING SHOWN. This viewport used to
   * draw substitute boxes from an item count, so "nothing here" and "the graph
   * produced nothing" and "the mesh was too big to send" all looked identical -
   * and all three looked like geometry. They are three different messages now.
   */
  const caption = $derived.by(() => {
    const parts: string[] = []
    const shapes = geometry.meshes.length + geometry.lines.length + geometry.points.length
    if (shapes > 0) parts.push(`${shapes} shape${shapes === 1 ? '' : 's'}`)
    if (geometry.truncated) parts.push('too large to show here')
    if (geometry.nonGeometric > 0) parts.push(`${geometry.nonGeometric} non-geometric`)
    if (parts.length === 0) parts.push('nothing to show')
    return parts.join(' / ')
  })
</script>

<section
  class="nodrag nowheel"
  class:active
  aria-label="Node geometry preview"
  onpointerdown={(event) => active && event.stopPropagation()}
  onpointermove={(event) => active && event.stopPropagation()}
  onwheel={(event) => active && event.stopPropagation()}
>
  <Canvas dpr={1}>
    <ViewerScene {geometry} {representation} {color} {active} {fitToken} {resetToken} />
  </Canvas>
  {#if empty}
    <p>{caption}</p>
  {/if}
  <span>{caption}</span>
  <ViewerToolbar {active} {onactive} onfit={() => (fitToken += 1)} onreset={() => (resetToken += 1)} />
</section>

<style>
  section {
    position: relative;
    height: 100%;
    overflow: hidden;
    border: 1px solid #303945;
    border-radius: 4px;
    background: radial-gradient(circle at 50% 35%, #26313b 0%, #0c1014 72%);
  }

  /*
   * Taken OUT OF FLOW, so the canvas can never contribute to the height of the
   * box it is measuring itself against. In flow it did, and the node grew a
   * little on every frame for as long as it stayed selected. NodeBody gives
   * this section a definite height; the canvas just fills it.
   */
  section :global(canvas) {
    position: absolute;
    inset: 0;
    display: block;
    width: 100% !important;
    height: 100% !important;
  }

  p {
    position: absolute;
    top: 50%;
    left: 0;
    width: 100%;
    margin: 0;
    color: #6d7b89;
    font: 9px/1 'Segoe UI', sans-serif;
    text-align: center;
    transform: translateY(-50%);
    pointer-events: none;
  }

  span {
    position: absolute;
    right: 7px;
    bottom: 6px;
    padding: 3px 5px;
    border-radius: 2px;
    background: rgb(8 11 14 / 72%);
    color: #aeb9c5;
    font: 8px/1 ui-monospace, monospace;
    pointer-events: none;
  }
  section:not(.active) :global(canvas) { pointer-events: none; }
  section.active { border-color: var(--node-color); }
</style>
