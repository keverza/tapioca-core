<script lang="ts">
  interface ContextAction {
    label: string
    disabled?: boolean
    run: () => void
  }

  let {
    x,
    y,
    label,
    actions,
    onclose,
  }: { x: number; y: number; label: string; actions: ContextAction[]; onclose: () => void } = $props()

  function invoke(action: ContextAction): void {
    if (action.disabled) return
    onclose()
    action.run()
  }
</script>

<div class="context-menu nodrag nowheel" style={`left:${x}px; top:${y}px`} role="menu" aria-label={label}>
  <header>{label}</header>
  {#each actions as action}
    <button role="menuitem" type="button" disabled={action.disabled} onclick={() => invoke(action)}>{action.label}</button>
  {/each}
</div>
