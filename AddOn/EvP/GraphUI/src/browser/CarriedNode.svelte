<script lang="ts">
  /**
   * The node between being chosen and being placed - it follows the pointer
   * until the user says where it goes.
   *
   * ⚠️ THIS IS THE REPLACEMENT FOR HTML5 DRAG-AND-DROP, AND IT IS NOT THE SAME
   * GESTURE WEARING A DIFFERENT HAT. `dragstart`/`drop` could only ever begin
   * on a mouse press, gave no say over what the cursor showed, and dropped the
   * whole gesture the moment the source element unmounted - which is exactly
   * what happens when the browser closes on the press. Pointer events on the
   * window survive the dialog closing, and let BOTH placements work:
   *
   *   press, move, release   → placed where the button came up
   *   press, release         → still carried; the next click places it
   *
   * The second is TouchDesigner's, and it is the one that also works from a
   * trackpad or after the keyboard picked the node.
   */
  import { untrack } from 'svelte'
  import { categoryColor } from '../nodes/types/display'
  import type { NodeTypeSchema } from '../types'

  let {
    schema,
    origin,
    onplace,
    oncancel,
  }: {
    schema: NodeTypeSchema
    /** The press that started the carry, in client coordinates. */
    origin: { x: number; y: number }
    onplace: (point: { x: number; y: number }) => void
    oncancel: () => void
  } = $props()

  /** Pixels of travel below which the press counts as a click, not a drag. */
  const DRAG_THRESHOLD = 4

  // `untrack`: the origin is a starting point, not a position to follow. Once
  // the pointer has moved the chip is where the pointer is, and re-seeding it
  // from the prop would snap it back to where the press happened.
  let x = $state(untrack(() => origin.x))
  let y = $state(untrack(() => origin.y))
  // Set when the first press is released without travel: from then on the node
  // rides the cursor and the NEXT press places it.
  let riding = $state(false)

  function move(event: PointerEvent): void {
    x = event.clientX
    y = event.clientY
  }

  function up(event: PointerEvent): void {
    if (riding) return
    const travelled = Math.hypot(event.clientX - origin.x, event.clientY - origin.y)
    if (travelled < DRAG_THRESHOLD) {
      riding = true
      return
    }
    onplace({ x: event.clientX, y: event.clientY })
  }

  /**
   * The press that places a riding node. `capture`, so it lands before the
   * canvas reads it as the start of a pan or a selection rectangle.
   *
   * The right button is RESERVED rather than bound to cancel: it is being kept
   * for a decision not yet made, so it is swallowed here and does nothing. Esc
   * cancels.
   */
  function down(event: PointerEvent): void {
    if (!riding) return
    event.preventDefault()
    event.stopPropagation()
    if (event.button === 0) onplace({ x: event.clientX, y: event.clientY })
  }

  function keydown(event: KeyboardEvent): void {
    if (event.key !== 'Escape') return
    event.preventDefault()
    event.stopPropagation()
    oncancel()
  }

  function block(event: MouseEvent): void {
    event.preventDefault()
  }

  // The two capture-phase listeners are registered by hand rather than through
  // `<svelte:window>`, because they only work if they run BEFORE the canvas
  // sees the same event, and that ordering is the whole reason they exist.
  $effect(() => {
    window.addEventListener('pointerdown', down, { capture: true })
    window.addEventListener('keydown', keydown, { capture: true })
    return () => {
      window.removeEventListener('pointerdown', down, { capture: true })
      window.removeEventListener('keydown', keydown, { capture: true })
    }
  })
</script>

<svelte:window
  onpointermove={move}
  onpointerup={up}
  onpointercancel={oncancel}
  oncontextmenu={block}
/>

<div
  class="carried"
  style={`left:${x}px; top:${y}px; --node-color: ${categoryColor(schema.category)}`}
  aria-hidden="true"
>
  <span class="type-mark"></span>
  <strong>{schema.label}</strong>
</div>

<style>
  /*
    `fixed` and pointer-transparent: the chip is drawn in client coordinates
    because that is what the pointer reports, and it must never become the
    target of the press that is meant to place it.
  */
  .carried { position: fixed; z-index: 60; display: flex; align-items: center; padding: 0 9px 0 7px; border: 1px solid var(--node-color); border-radius: 3px; background: var(--surface-raised); box-shadow: 0 10px 26px rgb(0 0 0 / 45%); gap: 6px; min-height: 24px; opacity: 0.92; pointer-events: none; transform: translate(10px, 10px); }
  .type-mark { width: 7px; height: 7px; transform: rotate(45deg); border: 1px solid var(--node-color); background: color-mix(in srgb, var(--node-color) 22%, transparent); }
  strong { color: var(--text); font-size: 11px; font-weight: 500; white-space: nowrap; }
</style>
