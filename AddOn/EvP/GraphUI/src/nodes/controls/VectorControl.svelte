<script lang="ts">
  /**
   * A point3, as one row of component fields - the nTop shape the design
   * reference asks for.
   *
   * ONE ROW, NOT THREE PORTS, and that is the design decision worth keeping: the
   * value is a single Point3 parameter under the input port's own id, so the
   * same row is an editable triple when nothing is wired and the upstream value
   * when something is. Three x/y/z ports would have made that impossible and
   * would have put three handles where the user expects one.
   *
   * Every field commits the WHOLE point, because that is what the value is; a
   * per-component edit would need a native read-modify-write and could lose a
   * concurrent change to a sibling component.
   */
  import type { GraphValue } from '../../types'
  import { componentLabels, componentsOf, formatComponents, formatNumber } from './widgets'
  import type { ParameterUi } from '../../types'

  let {
    value,
    ui,
    label,
    disabled = false,
    oncommit,
  }: {
    value: GraphValue | undefined
    ui: ParameterUi | undefined
    label: string
    disabled?: boolean
    oncommit: (text: string) => void
  } = $props()

  const labels = $derived(componentLabels(ui))
  const components = $derived(componentsOf(value, labels.length))
  const decimals = $derived(ui?.decimals)
  // Per-field drafts, so typing in Y does not reformat X under the cursor.
  let drafts = $state<(string | undefined)[]>([])

  function commit(index: number, raw: string): void {
    const parsed = Number(raw.trim())
    drafts[index] = undefined
    if (raw.trim() === '' || !Number.isFinite(parsed)) return
    if (parsed === components[index]) return
    const next = components.slice()
    next[index] = parsed
    oncommit(formatComponents(next, decimals))
  }
</script>

<div class="vector" style={`--axes: ${labels.length}`}>
  {#each labels as axis, index (axis)}
    <label class="axis nodrag">
      <span>{axis}</span>
      <input
        type="text"
        inputmode="decimal"
        {disabled}
        aria-label={`${label} ${axis}`}
        value={drafts[index] ?? formatNumber(components[index], decimals)}
        onfocus={(event) => (drafts[index] = event.currentTarget.value)}
        oninput={(event) => (drafts[index] = event.currentTarget.value)}
        onblur={(event) => commit(index, event.currentTarget.value)}
        onkeydown={(event) => {
          if (event.key === 'Enter') event.currentTarget.blur()
          else if (event.key === 'Escape') { drafts[index] = undefined; event.currentTarget.blur() }
        }}
      />
    </label>
  {/each}
</div>

<style>
  .vector { display: grid; grid-template-columns: repeat(var(--axes), minmax(0, 1fr)); gap: 2px; }
  /*
   * The axis letter sits INSIDE the field's box rather than above it, so three
   * components cost one row instead of two - a node body is short, and a point
   * that took two rows would push every other parameter off the visible node.
   */
  .axis { position: relative; display: block; }
  .axis > span { position: absolute; top: 50%; left: 4px; color: var(--text-faint); font: 7px/1 ui-monospace, monospace; transform: translateY(-50%); pointer-events: none; }
  .axis input { width: 100%; min-width: 0; height: 19px; padding: 0 4px 0 13px; border: 1px solid transparent; border-radius: 2px; background: var(--canvas); color: var(--text); font: 9px/1.2 ui-monospace, monospace; text-align: right; }
  .axis input:hover:not(:disabled) { border-color: var(--border); }
  .axis input:focus { border-color: var(--node-color); outline: none; }
  .axis input:disabled { color: var(--text-faint); }
</style>
