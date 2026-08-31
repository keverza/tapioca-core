<script lang="ts">
  import { Canvas } from '@threlte/core'
  import type { NodeResultRecord } from '../types'
  import ViewerScene from './viewer/ViewerScene.svelte'
  import ViewerToolbar from './viewer/ViewerToolbar.svelte'

  let { result, active, onactive }: { result?: NodeResultRecord; active: boolean; onactive: () => void } = $props()
</script>

<section class="nodrag nowheel" aria-label="Native result 3D preview">
  <Canvas dpr={1}>
    <ViewerScene
      itemCount={result?.itemCount ?? 0}
      status={result?.status ?? 'pending'}
    />
  </Canvas>
  <span>{result?.itemCount ?? 0} items</span>
  <ViewerToolbar {active} {onactive} />
</section>

<style>
  section {
    position: relative;
    height: 150px;
    margin: 0 10px 10px;
    overflow: hidden;
    border: 1px solid #303945;
    border-radius: 4px;
    background: radial-gradient(circle at 50% 35%, #26313b 0%, #0c1014 72%);
  }

  section :global(canvas) {
    width: 100% !important;
    height: 100% !important;
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
</style>
