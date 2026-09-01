<script lang="ts">
  /**
   * A number: a track to drag, a field to type in, and a stepper to nudge.
   *
   * ONE COMPONENT FOR ALL THREE, because a slider is not a different kind of
   * control - it is `number` that happens to have two ends. A slider whose
   * bounds are themselves parameters can LOSE them while the user clears the
   * minimum box, and splitting the two would make that moment a component swap:
   * the field remounting, and the focus going with it, mid-edit. Here the track
   * simply stops being drawn.
   *
   * ⚠️ THE DRAG DOES NOT COMMIT. A native graph edit per pixel would bump the
   * document revision and dirty everything downstream on every frame; the draft
   * is local until pointer-up or Enter. See HANDOFF-NodeGraphUIBuilder.md 5.2.
   */
  import { clampNumber, formatNumber, isDraggable, stepFor, type NumericRange } from './widgets'

  let {
    value,
    range,
    label,
    disabled = false,
    slider = false,
    oncommit,
  }: {
    value: number | undefined
    range: NumericRange
    label: string
    disabled?: boolean
    slider?: boolean
    oncommit: (text: string) => void
  } = $props()

  // The value being dragged or typed, or undefined when the control is showing
  // what the graph holds. Never initialised from `value`: doing that turns an
  // incoming canonical update into a stale draft the user did not type.
  let draft = $state<number | undefined>(undefined)
  let typing = $state<string | undefined>(undefined)

  const draggable = $derived(slider && isDraggable(range))
  const shown = $derived(draft ?? value ?? range.minimum ?? 0)
  const text = $derived(typing ?? formatNumber(draft ?? value, range.decimals))
  const step = $derived(stepFor(range))
  // A percentage, so the filled part of the track can be painted without
  // measuring anything - the same number the thumb sits at.
  const progress = $derived.by(() => {
    if (!draggable) return 0
    const span = (range.maximum as number) - (range.minimum as number)
    return span === 0 ? 0 : ((shown - (range.minimum as number)) / span) * 100
  })

  function commit(next: number): void {
    draft = undefined
    typing = undefined
    const clamped = clampNumber(next, range)
    if (value !== undefined && clamped === value) return
    oncommit(formatNumber(clamped, range.decimals))
  }

  function commitText(raw: string): void {
    const parsed = Number(raw.trim())
    if (raw.trim() === '' || !Number.isFinite(parsed)) {
      // Nothing usable: drop the draft and show the graph's value again rather
      // than leaving a box that says something the document does not.
      typing = undefined
      draft = undefined
      return
    }
    commit(parsed)
  }

  /**
   * One nudge. Rounded to the step's own precision before committing, because
   * repeated floating-point addition drifts - 0.1 nudged three times is
   * 0.30000000000000004, and that is what the user would then see in the box.
   */
  function nudge(direction: 1 | -1): void {
    const from = draft ?? value ?? range.minimum ?? 0
    const next = from + direction * step
    const decimals = range.decimals ?? Math.min(10, (String(step).split('.')[1] ?? '').length)
    commit(Number(next.toFixed(decimals)))
  }
</script>

<div class="number" class:with-slider={draggable}>
  {#if draggable}
    <input
      class="slider nodrag"
      type="range"
      min={range.minimum}
      max={range.maximum}
      {step}
      {disabled}
      value={shown}
      aria-label={`${label} slider`}
      style={`--progress: ${progress}%`}
      oninput={(event) => (draft = Number(event.currentTarget.value))}
      onchange={(event) => commit(Number(event.currentTarget.value))}
    />
  {/if}
  <div class="entry">
    <input
      class="field nodrag"
      type="text"
      inputmode="decimal"
      value={text}
      {disabled}
      aria-label={`${label} value`}
      onfocus={(event) => (typing = event.currentTarget.value)}
      oninput={(event) => (typing = event.currentTarget.value)}
      onblur={(event) => commitText(event.currentTarget.value)}
      onkeydown={(event) => {
        if (event.key === 'Enter') event.currentTarget.blur()
        else if (event.key === 'Escape') { typing = undefined; draft = undefined; event.currentTarget.blur() }
        else if (event.key === 'ArrowUp') { event.preventDefault(); nudge(1) }
        else if (event.key === 'ArrowDown') { event.preventDefault(); nudge(-1) }
      }}
    />
    <!-- The arrows are `tabindex=-1` on purpose: the field is already in the tab
         order and Up/Down do the same thing from the keyboard, so a second and
         third stop per number would triple the cost of tabbing across a node. -->
    <span class="stepper">
      <button type="button" tabindex="-1" {disabled} aria-label={`Increase ${label}`} onclick={() => nudge(1)}>⌃</button>
      <button type="button" tabindex="-1" {disabled} aria-label={`Decrease ${label}`} onclick={() => nudge(-1)}>⌄</button>
    </span>
  </div>
</div>

<style>
  .number { display: grid; align-items: center; gap: 4px; }
  .with-slider { grid-template-columns: minmax(0, 1fr) 60px; }
  .entry { display: grid; grid-template-columns: minmax(0, 1fr) 11px; align-items: stretch; border: 1px solid transparent; border-radius: 2px; background: var(--canvas); }
  .entry:hover { border-color: var(--border); }
  .entry:focus-within { border-color: var(--node-color); }
  .field { width: 100%; min-width: 0; height: 19px; padding: 0 4px; border: 0; background: transparent; color: var(--text); font: 9px/1.2 ui-monospace, monospace; text-align: right; }
  .field:focus { outline: none; }
  .field:disabled { color: var(--text-faint); }

  .stepper { display: grid; grid-template-rows: 1fr 1fr; border-left: 1px solid color-mix(in srgb, var(--border) 60%, transparent); }
  .stepper button { display: grid; width: 11px; height: auto; min-height: 0; padding: 0; border: 0; border-radius: 0; background: transparent; color: var(--text-faint); font-size: 7px; line-height: 1; place-items: center; cursor: pointer; }
  .stepper button:hover:not(:disabled) { background: var(--surface-hover); color: var(--text); }
  .stepper button:disabled { cursor: default; opacity: .4; }

  /*
   * The track is drawn by the input itself rather than by a wrapper, so the
   * filled part and the thumb cannot drift apart while dragging. `--progress`
   * is set from the SHOWN value, which is the draft during a drag - that is what
   * makes the fill follow the thumb before anything is committed.
   */
  .slider { width: 100%; height: 15px; padding: 0; border: 0; border-radius: 0; margin: 0; background: transparent; appearance: none; cursor: ew-resize; }
  .slider:disabled { cursor: default; opacity: .5; }
  .slider::-webkit-slider-runnable-track { height: 4px; border-radius: 2px; background: linear-gradient(to right, var(--accent) var(--progress), var(--border) var(--progress)); }
  .slider::-webkit-slider-thumb { width: 12px; height: 12px; margin-top: -4px; border: 2px solid var(--surface); border-radius: 50%; background: var(--accent); appearance: none; }
  .slider:focus-visible { outline: 1px solid var(--node-color); outline-offset: 1px; }
</style>
