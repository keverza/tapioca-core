<script lang="ts">
  /**
   * Where a node's preview is drawn, as a segmented switch.
   *
   * ⚠️ SEGMENTED RATHER THAN A DROPDOWN, because all three choices are visible
   * at once and the current one is visible WITHOUT opening anything. This is a
   * control someone flips while looking at the model rather than at the node,
   * and a dropdown makes "which one am I on" cost a click.
   *
   * The options are the runtime's own, in the runtime's order, and this draws
   * whatever it is given: the native registration pins the three spellings, so a
   * client that invented its own labels would be the thing that could disagree.
   */
  import type { GraphValue, ParameterOption } from '../../types'
  import { optionKey, textOf } from './widgets'

  let {
    value,
    options,
    label,
    disabled = false,
    oncommit,
  }: {
    value: GraphValue | undefined
    options: ParameterOption[]
    label: string
    disabled?: boolean
    oncommit: (text: string) => void
  } = $props()

  const current = $derived(textOf(value))
</script>

<div class="segments" role="radiogroup" aria-label={label}>
  {#each options as option (optionKey(option.value))}
    {@const text = option.value.text ?? ''}
    <button
      type="button"
      role="radio"
      aria-checked={text === current}
      class:on={text === current}
      {disabled}
      title={`Draw in: ${option.label}`}
      onclick={() => text !== current && oncommit(text)}
    >{option.label}</button>
  {/each}
</div>

<style>
  .segments { display: flex; overflow: hidden; min-width: 0; border: 1px solid var(--border); border-radius: 3px; }
  button { min-width: 0; flex: 1 1 0; padding: 0 2px; border: 0; border-left: 1px solid var(--border); background: var(--canvas); color: var(--text-muted); font: 9px/17px 'Segoe UI', sans-serif; cursor: pointer; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  button:first-child { border-left: 0; }
  button:hover:not(:disabled) { background: var(--surface); color: var(--text); }
  /* The selected segment is filled, not merely outlined: at 17px an outline and
     a hover state are the same picture, and this control is read at a glance. */
  button.on { background: var(--accent, #4c8cd8); color: #fff; }
  button:disabled { cursor: default; opacity: .5; }
</style>
