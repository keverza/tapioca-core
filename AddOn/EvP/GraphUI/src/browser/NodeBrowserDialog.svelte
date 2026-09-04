<script lang="ts">
  /**
   * The node browser: search, category tabs, a dense assortment and a preview.
   *
   * This REPLACED the docked component rail and its drag-and-drop. The rail cost
   * a permanent quarter of the canvas to show a list that is only wanted for the
   * two seconds before a node is created, and its HTML5 drag gesture could not
   * be started from the keyboard at all. The interaction model is TouchDesigner's
   * OP Create dialog: open at the cursor, type, place.
   *
   * Nothing here knows how a node is made. Picking one calls back with the
   * schema; GraphEditor owns the runtime edit and the carried-node gesture.
   */
  import { closeOnOutsidePress } from '../interaction'
  import { categoryColor } from '../nodes/types/display'
  import type { NodeTypeSchema } from '../types'
  import NodePreview from './NodePreview.svelte'
  import { ALL_CATEGORY, browserCategories, moveSelection, rankCatalog } from './nodeBrowser'

  let {
    catalog,
    x,
    y,
    busy = false,
    oncarry,
    oncreate,
    onclose,
    onreserved,
  }: {
    catalog: NodeTypeSchema[]
    x: number
    y: number
    busy?: boolean
    /** Press-and-hold: the node leaves the dialog and follows the pointer. */
    oncarry: (schema: NodeTypeSchema, event: PointerEvent) => void
    /** Keyboard path: create at the position the dialog was opened on. */
    oncreate: (schema: NodeTypeSchema) => void
    onclose: () => void
    /** Right button on an entry. Reserved; see `reserveRightButton`. */
    onreserved?: (schema: NodeTypeSchema, event: MouseEvent) => void
  } = $props()

  /**
   * Rows per column of the assortment. A CONSTANT, not measured: the arrow keys
   * step by exactly this number, so the value the layout uses and the value the
   * keyboard uses must be the same one. `--rows` in the CSS reads it back.
   */
  const ROWS = 13

  let query = $state('')
  let category = $state(ALL_CATEGORY)
  let activeIndex = $state(0)
  let field = $state<HTMLInputElement>()

  const categories = $derived(browserCategories(catalog))
  const matches = $derived(rankCatalog(catalog, query, category))
  const active = $derived(matches[activeIndex])

  // Any change of what is listed re-aims at the first result: after typing a
  // character, Enter must mean "the best match for what I have typed now".
  $effect(() => {
    query
    category
    activeIndex = 0
  })

  $effect(() => {
    queueMicrotask(() => field?.focus())
  })

  function key(event: KeyboardEvent): void {
    if (event.key === 'Escape') {
      onclose()
    } else if (event.key === 'Enter') {
      if (active === undefined || busy) return
      // Create BEFORE closing: the caller reads the frozen open position off
      // the same request object that closing clears.
      oncreate(active)
      onclose()
    } else if (
      event.key === 'ArrowUp' ||
      event.key === 'ArrowDown' ||
      event.key === 'ArrowLeft' ||
      event.key === 'ArrowRight'
    ) {
      activeIndex = moveSelection(activeIndex, matches.length, ROWS, event.key)
    } else {
      return
    }
    event.preventDefault()
    // The canvas listens for Escape and Delete on the window. A dialog that let
    // its keys through would clear the tool selection behind itself.
    event.stopPropagation()
  }

  /**
   * Press an entry and it comes with you.
   *
   * `pointerdown`, not `click`: the whole point is that the placement is chosen
   * AFTER the press, while the button is still down. `preventDefault` stops the
   * press from also starting a text selection across the list.
   */
  function press(event: PointerEvent, schema: NodeTypeSchema): void {
    if (busy || event.button !== 0) return
    event.preventDefault()
    activeIndex = matches.indexOf(schema)
    oncarry(schema, event)
    onclose()
  }

  /**
   * The right button over an entry does nothing yet, ON PURPOSE.
   *
   * It is spoken for - per-node actions off the browser are still being decided
   * - and the browser is inside the canvas, whose own right-click opens the pane
   * context menu. Swallowing it here means the gesture is already isolated when
   * it is given a meaning, rather than the canvas menu appearing over the list
   * today and having to be un-taught later.
   */
  function reserveRightButton(event: MouseEvent, schema: NodeTypeSchema): void {
    event.preventDefault()
    event.stopPropagation()
    onreserved?.(schema, event)
  }
</script>

<div
  class="node-browser nodrag nowheel"
  style={`left:${x}px; top:${y}px; --rows:${ROWS}`}
  role="dialog"
  tabindex="-1"
  aria-label="Create node"
  use:closeOnOutsidePress={onclose}
  onkeydown={key}
>
  <header>
    <strong>Create Node</strong>
    <button type="button" onclick={onclose} aria-label="Close node browser">&#x00d7;</button>
  </header>

  <input
    bind:this={field}
    bind:value={query}
    type="search"
    placeholder="Search nodes"
    aria-label="Search nodes"
    autocomplete="off"
  />

  <div class="tabs" role="tablist" aria-label="Node categories">
    {#each categories as name}
      <button
        type="button"
        role="tab"
        aria-selected={name === category}
        class:selected={name === category}
        style={`--tab-color: ${name === ALL_CATEGORY ? 'var(--accent)' : categoryColor(name)}`}
        onclick={() => (category = name)}
      >{name}</button>
    {/each}
  </div>

  <div class="body">
    {#if matches.length === 0}
      <p class="empty">No node matches "{query}".</p>
    {:else}
      <div class="assortment" role="listbox" aria-label="Nodes" tabindex="-1">
        {#each matches as schema, index}
          <button
            type="button"
            role="option"
            aria-selected={index === activeIndex}
            class:active={index === activeIndex}
            disabled={busy}
            style={`--entry-color: ${categoryColor(schema.category)}`}
            onpointerdown={(event) => press(event, schema)}
            oncontextmenu={(event) => reserveRightButton(event, schema)}
            onpointerenter={() => (activeIndex = index)}
            onfocus={() => (activeIndex = index)}
          >
            <span class="dot" aria-hidden="true"></span>
            <span class="entry-label">{schema.label}</span>
          </button>
        {/each}
      </div>
    {/if}
    <NodePreview schema={active} />
  </div>

  <footer>
    <span>{matches.length} of {catalog.length}</span>
    <span>Press and hold to place / Enter drops at the cursor / Esc closes</span>
  </footer>
</div>

<style>
  .node-browser { position: absolute; z-index: 24; display: grid; width: 720px; padding: 7px; border: 1px solid var(--border); border-radius: 4px; background: var(--surface); box-shadow: 0 20px 48px rgb(0 0 0 / 52%); gap: 6px; grid-template-rows: auto auto auto minmax(0, 1fr) auto; }

  header { display: flex; align-items: center; justify-content: space-between; padding: 0 2px 1px; }
  header strong { color: var(--text-faint); font: 700 8px/1 ui-monospace, monospace; letter-spacing: 0.12em; text-transform: uppercase; }
  header button { width: 18px; height: 18px; padding: 0; border: 0; border-radius: 2px; background: transparent; color: var(--text-faint); font-size: 12px; cursor: pointer; }
  header button:hover { background: var(--surface-hover); color: var(--text); }

  input { width: 100%; height: 32px; box-sizing: border-box; padding: 0 9px; border: 1px solid var(--border); border-radius: 3px; background: var(--canvas); color: var(--text); font-size: 12px; }
  input:focus-visible { border-color: var(--accent); outline: none; }

  .tabs { display: flex; overflow-x: auto; flex-wrap: nowrap; gap: 3px; }
  .tabs button { height: 26px; flex: none; padding: 0 9px; border: 1px solid transparent; border-bottom: 2px solid transparent; border-radius: 2px 2px 0 0; background: transparent; color: var(--text-muted); font-size: 10px; cursor: pointer; white-space: nowrap; }
  .tabs button:hover { background: var(--surface-hover); color: var(--text); }
  .tabs button.selected { border-bottom-color: var(--tab-color); background: var(--surface-raised); color: var(--text); }

  .body { display: grid; overflow: hidden; height: 350px; border: 1px solid var(--border); border-radius: 3px; grid-template-columns: minmax(0, 1fr) 214px; }

  /*
    Columns of a fixed number of rows, filled downwards then rightwards, rather
    than one tall scrolling list. It is the reference's density, and it is what
    makes the left/right arrows mean something: index i is at column ⌊i/ROWS⌋.
  */
  .assortment { display: grid; overflow-x: auto; overflow-y: hidden; padding: 4px; gap: 0 6px; grid-auto-columns: minmax(150px, 1fr); grid-auto-flow: column; grid-template-rows: repeat(var(--rows), 25px); }
  .assortment button { display: flex; overflow: hidden; height: 25px; align-items: center; padding: 0 6px; border: 0; border-radius: 2px; background: transparent; color: var(--text-muted); font-size: 11px; gap: 6px; text-align: left; cursor: pointer; }
  .assortment button.active { background: var(--surface-hover); color: var(--text); }
  .assortment button:disabled { cursor: default; opacity: 0.5; }
  .dot { width: 5px; height: 5px; flex: none; border-radius: 50%; background: var(--entry-color); }
  .entry-label { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }

  .empty { display: grid; margin: 0; padding: 0 16px; color: var(--text-faint); font-size: 11px; place-content: center; text-align: center; }

  footer { display: flex; justify-content: space-between; padding: 0 2px; color: var(--text-faint); font: 9px/1 ui-monospace, monospace; gap: 10px; }
</style>
