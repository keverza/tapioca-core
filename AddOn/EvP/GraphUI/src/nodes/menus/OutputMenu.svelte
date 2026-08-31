<script lang="ts">
  import type { PortCapabilities } from '../types/port'
  let { capabilities, oncopy, onclose }: { capabilities: PortCapabilities; oncopy: () => void; onclose: () => void } = $props()
  const nativeReason = 'Requires a native expected-revision graph edit'
</script>

<div class="port-menu nodrag" role="menu">
  <button type="button" disabled={!capabilities.copyReference} onclick={() => { oncopy(); onclose() }}>Copy reference</button>
  <button type="button" disabled title={nativeReason}>Promote as graph output</button>
  <button type="button" disabled title="Requires native runtime value inspection">Inspect data</button>
  <button type="button" disabled title={nativeReason}>Set as display output</button>
  <hr />
  {#each capabilities.transforms as transform}<button type="button" disabled title={nativeReason}>{transform}</button>{/each}
</div>

<style>
  .port-menu { position: absolute; z-index: 20; top: 18px; right: 0; display: grid; min-width: 155px; padding: 4px; border: 1px solid var(--border); background: var(--surface); box-shadow: 0 12px 28px rgb(0 0 0 / 45%); }
  button { height: 25px; padding: 0 7px; border: 0; background: transparent; font-size: 8px; text-align: left; text-transform: capitalize; }
  hr { width: 100%; margin: 3px 0; border: 0; border-top: 1px solid var(--border); }
</style>
