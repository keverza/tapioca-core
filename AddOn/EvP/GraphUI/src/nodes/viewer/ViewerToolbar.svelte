<script lang="ts">
  let { active, onactive, onfit, onreset }: { active: boolean; onactive: () => void; onfit: () => void; onreset: () => void } = $props()
</script>

<div class="toolbar nodrag">
  <button type="button" onclick={onactive}>{active ? 'Return to graph' : 'Activate viewer'}</button>
  <button type="button" onclick={onfit}>Fit</button>
  <button type="button" onclick={onreset}>Reset</button>
</div>

<style>
  .toolbar { position: absolute; right: 5px; bottom: 5px; display: flex; gap: 3px; }
  button { height: 22px; padding: 0 6px; background: rgb(8 11 14 / 76%); color: #aeb9c5; font-size: 7px; }
</style>
