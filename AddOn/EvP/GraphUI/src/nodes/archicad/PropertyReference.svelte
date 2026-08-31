<script lang="ts">
  let { name, value, type }: { name: string; value: string; type: string } = $props()
</script>

<div>
  <button type="button" title="Copy reference support requires the native property selector">{name}</button>
  <span>{value}</span><small>{type}</small>
</div>

<style>
  div { display: grid; grid-template-columns: minmax(90px, 1fr) minmax(70px, 1fr); align-items: center; min-height: 27px; border-bottom: 1px solid color-mix(in srgb, var(--border) 55%, transparent); }
  button { overflow: hidden; height: 25px; padding: 0 7px; border: 0; background: transparent; font-size: 9px; text-align: left; text-overflow: ellipsis; white-space: nowrap; }
  span { overflow: hidden; color: var(--text-muted); font: 9px/1 ui-monospace, monospace; text-overflow: ellipsis; white-space: nowrap; }
  small { grid-column: 1 / -1; padding: 0 7px 4px; color: var(--text-faint); font: 7px/1 ui-monospace, monospace; }
</style>
