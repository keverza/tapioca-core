<script lang="ts">
  import { closeOnOutsidePress } from './interaction'

  interface ContextAction {
    label: string
    disabled?: boolean
    /**
     * Why the action is unavailable. The handoff's rule is that a disabled
     * action carries a reason rather than merely being grey, so this becomes
     * the item's tooltip and its accessible description.
     */
    title?: string
    /** Heading this item sits under. A new value opens a new section. */
    group?: string
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

<!--
  THE context menu. One component, one style, one at a time.
  Ports, nodes, edges and the canvas all open this: three menus in three sizes
  on screen at once is what happens when each thing draws its own.
-->
<div
  class="context-menu nodrag nowheel"
  style={`left:${x}px; top:${y}px`}
  role="menu"
  aria-label={label}
  use:closeOnOutsidePress={onclose}
>
  <header>{label}</header>
  {#each actions as action, index}
    {#if action.group !== undefined && action.group !== actions[index - 1]?.group}
      <div class="group" role="separator">{action.group}</div>
    {/if}
    <button
      role="menuitem"
      type="button"
      disabled={action.disabled}
      title={action.title}
      aria-label={action.title === undefined ? undefined : action.label + ' - ' + action.title}
      onclick={() => invoke(action)}>{action.label}</button>
  {/each}
</div>
