<script lang="ts">
  /**
   * An Archicad attribute picker: a swatch, a name, a search box and folders.
   *
   * WHY THIS REPLACED A `<select>`. A native `<option>` can hold text and
   * nothing else - no swatch, no folder, no second column - so a fill list read
   * as "25 %, 50 %, 75 %" and a composite list as a wall of near-identical
   * names. Archicad's own pickers solve this with a preview per row, a search
   * field and folder grouping, and a user who knows those dialogs should not
   * have to learn a second, worse idiom inside the graph editor.
   *
   * ⚠️ STILL NO MODEL ENUMERATION HERE. The rows arrive from the native
   * listing; this component filters, groups and draws them. It knows nothing
   * about layers or fills beyond what a row carries - including which listings
   * get the colour GRID, which is decided by the rows all being flat colours
   * rather than by anyone here knowing what a pen is.
   */
  import type { AttributeListing, GraphValue, ParameterOption, ParameterOptionSource } from '../../types'
  import AttributeSwatch from './AttributeSwatch.svelte'
  import {
    filterChoices,
    groupChoices,
    hasFolders,
    hasSwatches,
    isColorGrid,
    isKnownOption,
    isIconSize,
    optionKey,
    textOf,
    ICON_SIZES,
    PEN_GRID_COLUMNS,
    type AttributeChoice,
    type IconSize,
  } from './widgets'

  let {
    value,
    choices,
    listing,
    source,
    label,
    disabled = false,
    loading = false,
    oncommit,
    onrequest,
  }: {
    value: GraphValue | undefined
    choices: AttributeChoice[]
    listing?: AttributeListing
    source: ParameterOptionSource
    label: string
    disabled?: boolean
    loading?: boolean
    oncommit: (text: string) => void
    onrequest?: (source: ParameterOptionSource, penSet?: string) => void
  } = $props()

  // The project's pen tables, when this listing has any. Switching sets is a
  // READ: it re-lists from that table's own definition and never changes which
  // set the project uses, because restyling every drawing is not something
  // looking at a colour should do.
  const penSets = $derived(listing?.penSets ?? [])
  const penSet = $derived(listing?.penSet ?? '')

  const SIZE_KEY = 'tapioca.picker.iconSize'

  let open = $state(false)
  let query = $state('')
  let grouped = $state(true)
  let active = $state(0)
  let field = $state<HTMLButtonElement>()
  let search = $state<HTMLInputElement>()

  /**
   * The swatch size, remembered per viewer.
   *
   * A preference about how big pictures are is exactly the kind of thing that
   * belongs to the person looking at the screen and not to the workflow, so it
   * lives in localStorage and never touches graph state. Wrapped because the
   * accessor itself throws in a private window or with site data blocked, and a
   * picker that cannot open is a far worse failure than one that forgot a size.
   */
  function storedSize(): IconSize {
    try {
      const saved = window.localStorage?.getItem(SIZE_KEY)
      return isIconSize(saved) ? saved : 'medium'
    } catch {
      return 'medium'
    }
  }
  let iconSize = $state<IconSize>(storedSize())
  function setIconSize(next: IconSize): void {
    iconSize = next
    try {
      window.localStorage?.setItem(SIZE_KEY, next)
    } catch {
      // Nothing to do and nothing to say: the size still applies this session.
    }
  }
  const swatchPx = $derived(ICON_SIZES[iconSize])

  const current = $derived(optionKey(value))
  const options = $derived<ParameterOption[]>(choices.map((choice) => choice.option))
  const known = $derived(isKnownOption(options, value))
  const selected = $derived(choices.find((choice) => optionKey(choice.option.value) === current))
  const filtered = $derived(filterChoices(choices, query))
  const foldersExist = $derived(hasFolders(choices))
  const swatched = $derived(hasSwatches(choices))
  const gridable = $derived(isColorGrid(choices))
  // A colour listing opens as a grid; anything else has names worth reading.
  let asGrid = $state(true)
  const grid = $derived(gridable && asGrid)
  const groups = $derived(grouped && foldersExist ? groupChoices(filtered) : [{ folder: '', choices: filtered }])
  // The flat order the arrow keys walk, which must match what is on screen -
  // deriving it from the same groups is what keeps the two from disagreeing.
  const walk = $derived(groups.flatMap((group) => group.choices))

  $effect(() => {
    if (source !== 'none' && choices.length === 0) onrequest?.(source)
  })

  function choose(choice: AttributeChoice): void {
    open = false
    query = ''
    const chosen = optionKey(choice.option.value)
    if (chosen !== current) oncommit(chosen)
    field?.focus()
  }

  function toggle(): void {
    if (disabled) return
    open = !open
    if (open) {
      query = ''
      active = Math.max(0, walk.findIndex((choice) => optionKey(choice.option.value) === current))
      // The search box takes focus on open, so typing narrows the list without
      // a second click - the behaviour of the dialogs this mirrors.
      queueMicrotask(() => search?.focus())
    }
  }

  function keydown(event: KeyboardEvent): void {
    if (event.key === 'Escape') {
      event.stopPropagation()
      open = false
      field?.focus()
      return
    }
    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
      event.preventDefault()
      if (walk.length === 0) return
      const step = event.key === 'ArrowDown' ? 1 : -1
      active = (active + step + walk.length) % walk.length
      return
    }
    if (event.key === 'Enter') {
      event.preventDefault()
      const choice = walk[active]
      if (choice !== undefined) choose(choice)
    }
  }
</script>

<svelte:window
  onpointerdown={(event) => {
    // An outside press closes it. A picker is transient by definition, unlike
    // the inspector panels, which deliberately survive a click on the canvas.
    if (!open) return
    const target = event.target as HTMLElement | null
    if (target?.closest('.attribute-picker') === null) open = false
  }}
/>

<div class="attribute-picker">
  <button
    bind:this={field}
    type="button"
    class="field nodrag"
    class:unknown={!known}
    {disabled}
    aria-haspopup="listbox"
    aria-expanded={open}
    aria-label={label}
    onclick={toggle}
  >
    {#if selected?.row.preview !== undefined}
      <AttributeSwatch preview={selected.row.preview} size={12} />
    {/if}
    <span class="name">
      {#if !known}{textOf(value)} (not in this project){:else if selected !== undefined}{selected.option.label}{:else if loading}Listing…{:else if choices.length === 0}None in this project{:else}Choose…{/if}
    </span>
    <span class="caret">⌄</span>
  </button>

  {#if open}
    <div class="popover nodrag nowheel">
      {#if penSets.length > 0}
        <div class="pen-set">
          <span>Pen Set</span>
          <select
            value={penSet}
            aria-label="Pen set"
            onchange={(event) => onrequest?.(source, event.currentTarget.value || undefined)}
          >
            <!-- The empty value is the project's CURRENT set, and it is a real
                 choice rather than a placeholder - it is what the picker opens
                 on and what a user comes back to. -->
            <option value="">Project's pens</option>
            {#each penSets as name (name)}
              <option value={name}>{name}</option>
            {/each}
          </select>
        </div>
      {/if}
      <header>
        <label class="search">
          <span aria-hidden="true">⌕</span>
          <input
            bind:this={search}
            bind:value={query}
            type="text"
            placeholder={`Search ${label}`}
            aria-label={`Search ${label}`}
            onkeydown={keydown}
          />
        </label>
        {#if gridable}
          <!-- A colour listing is the one that reads better as a palette than
               as rows; the toggle is offered only where both make sense. -->
          <button type="button" class:active={grid} title="Colour grid" aria-pressed={grid} onclick={() => (asGrid = true)}>▦</button>
          <button type="button" class:active={!grid} title="List" aria-pressed={!grid} onclick={() => (asGrid = false)}>☰</button>
        {:else if foldersExist}
          <!-- The two view modes Archicad's own picker offers. Hidden entirely
               when the project files nothing, because a toggle between one
               arrangement and the same arrangement is noise. -->
          <button type="button" class:active={grouped} title="Group by folder" aria-pressed={grouped} onclick={() => (grouped = true)}>▤</button>
          <button type="button" class:active={!grouped} title="Flat list" aria-pressed={!grouped} onclick={() => (grouped = false)}>☰</button>
        {/if}
      </header>

      {#if swatched}
        <div class="sizes" role="group" aria-label="Icon size">
          {#each [['small', 'S'], ['medium', 'M'], ['large', 'L']] as [size, initial] (size)}
            <button
              type="button"
              class:active={iconSize === size}
              aria-pressed={iconSize === size}
              title={`${initial === 'S' ? 'Small' : initial === 'M' ? 'Medium' : 'Large'} icons`}
              onclick={() => setIconSize(size as IconSize)}
            >{initial}</button>
          {/each}
        </div>
      {/if}

      {#if grid}
        <!-- Archicad's pen palette: the colours themselves, picked directly.
             Sized from the same control the list uses, so "large icons" means
             one thing in both arrangements. -->
        <div
          class="grid"
          role="listbox"
          aria-label={label}
          tabindex="-1"
          style={`--columns: ${PEN_GRID_COLUMNS}`}
          onkeydown={keydown}
        >
          {#if filtered.length === 0}
            <p class="empty">{choices.length === 0 ? (loading ? 'Listing…' : 'Nothing to choose from') : 'No match'}</p>
          {:else}
            {#each filtered as choice (optionKey(choice.option.value))}
              {@const key = optionKey(choice.option.value)}
              <button
                type="button"
                role="option"
                aria-selected={key === current}
                aria-label={choice.option.label}
                class="cell"
                class:selected={key === current}
                style={`background: ${choice.row.preview?.color}`}
                title={choice.option.label}
                onclick={() => choose(choice)}
              ></button>
            {/each}
          {/if}
        </div>
      {:else}
        <div class="rows" role="listbox" aria-label={label} tabindex="-1" onkeydown={keydown}>
          {#if filtered.length === 0}
            <p class="empty">{choices.length === 0 ? (loading ? 'Listing…' : 'Nothing to choose from') : 'No match'}</p>
          {/if}
          {#each groups as group (group.folder)}
            {#if group.folder !== ''}
              <h4>{group.folder}</h4>
            {/if}
            {#each group.choices as choice (optionKey(choice.option.value))}
              {@const key = optionKey(choice.option.value)}
              <button
                type="button"
                role="option"
                aria-selected={key === current}
                class="row"
                class:selected={key === current}
                class:active={walk[active] === choice}
                class:dimmed={choice.row.hidden}
                onpointerenter={() => (active = walk.indexOf(choice))}
                onclick={() => choose(choice)}
              >
                {#if swatched}
                  <!-- The column exists only when something in this listing has
                       a picture; profiles and layers have none, and a row of
                       empty boxes reads as pictures that failed to load. -->
                  <span class="icon" style={`--icon: ${swatchPx * 1.7}px`}>
                    <AttributeSwatch preview={choice.row.preview} size={swatchPx} />
                  </span>
                {/if}
                <span class="label">{choice.option.label}</span>
                <!-- A hidden or locked layer is REPORTED, not filtered: it is
                     still a legal choice, and dropping it would read as the
                     layer having been deleted. -->
                {#if choice.row.hidden}<em title="Hidden layer">hidden</em>{/if}
                {#if choice.row.locked}<em title="Locked layer">locked</em>{/if}
                {#if choice.row.preview?.kind === 'surface'}
                  <!-- Colour on the left, hatch and texture on the right: the
                       arrangement Archicad's own surface list uses, and it earns
                       its width - a project's renders and plasters run to dozens
                       of near-identical colours, and the hatch is what separates
                       a roof tile from a render. -->
                  <span class="trailing" style={`--icon: ${swatchPx}px`}>
                    <AttributeSwatch preview={choice.row.preview} size={swatchPx} part="pattern" />
                  </span>
                  {#if choice.row.preview.hasTexture}
                    <span class="texture" title="Has a texture image" aria-label="Has a texture image">▨</span>
                  {/if}
                {/if}
                {#if choice.row.preview?.thickness !== undefined}
                  <small>{Math.round(choice.row.preview.thickness * 1000)}</small>
                {:else if choice.row.number !== undefined}
                  <small>{choice.row.number}</small>
                {/if}
              </button>
            {/each}
          {/each}
        </div>
      {/if}
    </div>
  {/if}
</div>

<style>
  .attribute-picker { position: relative; }
  /* `overflow: hidden` on the field, so nothing a swatch draws can spill past
     the row it belongs to. */
  .field { display: flex; overflow: hidden; width: 100%; height: 19px; align-items: center; padding: 0 3px 0 4px; border: 1px solid transparent; border-radius: 2px; background: var(--canvas); color: var(--text); font: 9px/1.2 'Segoe UI', sans-serif; gap: 5px; text-align: left; }
  .field:hover:not(:disabled) { border-color: var(--border); }
  .field[aria-expanded='true'] { border-color: var(--node-color); }
  .field:disabled { color: var(--text-faint); }
  .field.unknown { color: var(--danger); }
  .name { overflow: hidden; flex: 1; text-overflow: ellipsis; white-space: nowrap; }
  .caret { color: var(--text-faint); font-size: 9px; }

  /*
   * Above every node and its panels. The picker opens from a row that is itself
   * inside a node, so anything less puts the list behind the next node along -
   * and a list you cannot see all of is not a list. `isolation: isolate` keeps
   * the popover's own stacking self-contained inside the flow viewport's
   * transformed layer, where a stray composited box is otherwise free to paint
   * over it.
   */
  .popover { position: absolute; z-index: 30; top: calc(100% + 3px); left: 0; display: grid; width: max(230px, 100%); max-height: 300px; grid-template-rows: auto auto auto minmax(0, 1fr); isolation: isolate; overflow: hidden; border: 1px solid var(--border); border-radius: 3px; background: var(--surface-raised); box-shadow: 0 14px 34px rgb(0 0 0 / 45%); }
  .popover header { display: flex; align-items: center; padding: 5px; border-bottom: 1px solid var(--border); gap: 3px; }
  .search { display: flex; flex: 1; align-items: center; padding: 0 5px; border: 1px solid var(--border); border-radius: 2px; background: var(--canvas); gap: 4px; }
  .search span { color: var(--text-faint); font-size: 10px; }
  .search input { width: 100%; min-width: 0; height: 19px; padding: 0; border: 0; background: transparent; color: var(--text); font: 10px/1.2 'Segoe UI', sans-serif; }
  .search input:focus { outline: none; }
  .popover header > button { width: 21px; height: 21px; flex: none; padding: 0; border-color: transparent; background: transparent; color: var(--text-faint); font-size: 11px; }
  .popover header > button.active { border-color: var(--border); background: var(--surface-hover); color: var(--text); }

  .sizes { display: flex; justify-content: flex-end; padding: 3px 5px; border-bottom: 1px solid color-mix(in srgb, var(--border) 55%, transparent); gap: 2px; }
  .sizes button { width: 18px; height: 15px; padding: 0; border: 0; border-radius: 2px; background: transparent; color: var(--text-faint); font: 8px/1 ui-monospace, monospace; }
  .sizes button:hover { background: var(--surface-hover); color: var(--text); }
  .sizes button.active { background: var(--accent-soft); color: var(--accent); }

  .rows { overflow-y: auto; padding: 3px; }
  h4 { position: sticky; top: 0; z-index: 1; margin: 4px 0 2px; padding: 3px 5px; background: var(--surface-raised); color: var(--text-faint); font: 700 7px/1 ui-monospace, monospace; letter-spacing: .1em; text-transform: uppercase; }
  .row { display: flex; width: 100%; height: auto; min-height: 21px; align-items: center; padding: 2px 5px; border: 0; border-radius: 2px; background: transparent; color: var(--text); font: 10px/1.3 'Segoe UI', sans-serif; gap: 6px; text-align: left; }
  /* A fixed column, so names line up whatever each row's swatch happens to be -
     a line swatch is wider than a fill one. */
  .icon { display: flex; width: var(--icon); flex: none; align-items: center; justify-content: center; }
  /* Hover and keyboard share one highlight: two different "where am I" marks on
     one list is one too many. */
  .row.active { background: var(--surface-hover); }
  .row.selected { color: var(--accent); }
  .row.dimmed .label { opacity: .55; }
  .label { overflow: hidden; flex: 1; text-overflow: ellipsis; white-space: nowrap; }
  .row em { color: var(--text-faint); font: 7px/1 ui-monospace, monospace; font-style: normal; }
  .row small { color: var(--text-faint); font: 8px/1 ui-monospace, monospace; }

  /*
   * TWENTY COLUMNS, fixed, because that is how Archicad lays its pen palette
   * out: the pen NUMBERS are positions in that grid, so a user who knows where
   * pen 41 sits reaches for the same place here. Reflowing to fit the popover
   * would keep every colour and lose the map, which is the useful half.
   */
  .grid { display: grid; grid-template-columns: repeat(var(--columns), 1fr); align-content: start; padding: 5px; overflow-y: auto; gap: 1px; }
  .cell { width: 100%; aspect-ratio: 1; height: auto; min-height: 0; padding: 0; border: 1px solid var(--border); border-radius: 0; }
  .cell:hover { border-color: var(--text); }
  .cell.selected { box-shadow: 0 0 0 2px var(--accent); }
  .pen-set { display: flex; align-items: center; padding: 5px 5px 0; gap: 5px; }
  .pen-set span { color: var(--text-faint); font: 700 7px/1 ui-monospace, monospace; letter-spacing: .1em; text-transform: uppercase; }
  .pen-set select { width: 100%; min-width: 0; max-width: none; height: 19px; padding: 0 4px; border: 1px solid var(--border); border-radius: 2px; background: var(--canvas); color: var(--text); font: 9px/1.2 'Segoe UI', sans-serif; }
  .trailing { display: flex; width: var(--icon); flex: none; align-items: center; justify-content: center; }
  .texture { color: var(--text-faint); font-size: 10px; }
  .empty { grid-column: 1 / -1; width: 100%; margin: 0; padding: 14px 8px; color: var(--text-faint); font-size: 10px; text-align: center; }
</style>
