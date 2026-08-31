<script lang="ts">
  let { count, onclick }: { count: number; onclick: () => void } = $props()
</script>

<button type="button" {onclick}>
  <span>Archicad selection</span><strong>{count}</strong><small>Browse reference</small>
</button>

<style>
  button { display: grid; grid-template-columns: 1fr auto; width: 100%; height: auto; min-height: 42px; padding: 7px 8px; background: var(--canvas); text-align: left; }
  span { color: var(--text-muted); font-size: 9px; }
  strong { grid-row: 1 / 3; grid-column: 2; align-self: center; color: var(--node-color); font: 600 18px/1 ui-monospace, monospace; }
  small { color: var(--text-faint); font-size: 8px; }
</style>
