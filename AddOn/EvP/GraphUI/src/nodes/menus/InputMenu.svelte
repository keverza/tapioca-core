<script lang="ts">
  import type { PortCapabilities, PortTransform } from '../types/port'
  let { capabilities, transforms = [], oncopy, onclose }: { capabilities: PortCapabilities; transforms?: PortTransform[]; oncopy: () => void; onclose: () => void } = $props()
  const nativeReason = 'Requires a native expected-revision graph edit'
</script>

<div class="port-menu nodrag" role="menu">
  <button type="button" disabled={!capabilities.copyReference} onclick={() => { oncopy(); onclose() }}>Copy reference</button>
  <button type="button" disabled title={nativeReason}>Paste reference</button>
  <button type="button" disabled title={nativeReason}>Internalise</button>
  <button type="button" disabled title={nativeReason}>Promote parameter</button>
  <hr />
  {#each capabilities.transforms as transform}
    <button type="button" disabled title={nativeReason}>{transforms.some((item) => item.type === transform) ? 'Remove ' : ''}{transform}</button>
  {/each}
</div>

<style>
  .port-menu { position: absolute; z-index: 20; top: 18px; left: 0; display: grid; min-width: 145px; padding: 4px; border: 1px solid var(--border); background: var(--surface); box-shadow: 0 12px 28px rgb(0 0 0 / 45%); }
  button { height: 25px; padding: 0 7px; border: 0; background: transparent; font-size: 8px; text-align: left; text-transform: capitalize; }
  hr { width: 100%; margin: 3px 0; border: 0; border-top: 1px solid var(--border); }
</style>
