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
   * about layers or fills beyond what a row carries.
   *
   * The popover is positioned by the browser's own anchoring rules rather than
   * by measurement: it is absolutely placed against the field and allowed to
   * flip upward when the node sits low on the canvas.
   */
  import type { GraphValue, ParameterOption, ParameterOptionSource } from '../../types'
  import AttributeSwatch from './AttributeSwatch.svelte'
  import {
    filterChoices,
    groupChoices,
    hasFolders,
    isKnownOption,
    optionKey,
    textOf,
    type AttributeChoice,
  } from './widgets'

  let {
    value,
    choices,
    source,
    label,
    disabled = false,
    loading = false,
    oncommit,
    onrequest,
  }: {
    value: GraphValue | undefined
    choices: AttributeChoice[]
    source: ParameterOptionSource
    label: string
    disabled?: boolean
    loading?: boolean
    oncommit: (text: string) => void
    onrequest?: (source: ParameterOptionSource) => void
  } = $props()

  let open = $state(false)
  let query = $state('')
  let grouped = $state(true)
  let active = $state(0)
  let field = $state<HTMLButtonElement>()
  let search = $state<HTMLInputElement>()

  const current = $derived(optionKey(value))
  const options = $derived<ParameterOption[]>(choices.map((choice) => choice.option))
  const known = $derived(isKnownOption(options, value))
  const selected = $derived(choices.find((choice) => optionKey(choice.option.value) === current))
  const filtered = $derived(filterChoices(choices, query))
  const foldersExist = $derived(hasFolders(choices))
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
    <AttributeSwatch preview={selected?.row.preview} size={12} />
    <span class="name">
      {#if !known}{textOf(value)} (not in this project){:else if selected !== undefined}{selected.option.label}{:else if loading}Listing…{:else if choices.length === 0}None in this project{:else}Choose…{/if}
    </span>
    <span class="caret">⌄</span>
  </button>

  {#if open}
    <div class="popover nodrag nowheel">
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
        {#if foldersExist}
          <!-- The two view modes Archicad's own picker offers. Hidden entirely
               when the project files nothing, because a toggle between one
               arrangement and the same arrangement is noise. -->
          <button type="button" class:active={grouped} title="Group by folder" aria-pressed={grouped} onclick={() => (grouped = true)}>▤</button>
          <button type="button" class:active={!grouped} title="Flat list" aria-pressed={!grouped} onclick={() => (grouped = false)}>☰</button>
        {/if}
      </header>

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
              <AttributeSwatch preview={choice.row.preview} size={14} />
              <span class="label">{choice.option.label}</span>
              <!-- A hidden or locked layer is REPORTED, not filtered: it is
                   still a legal choice, and dropping it would read as the layer
                   having been deleted. -->
              {#if choice.row.hidden}<em title="Hidden layer">hidden</em>{/if}
              {#if choice.row.locked}<em title="Locked layer">locked</em>{/if}
              {#if choice.row.preview?.thickness !== undefined}
                <small>{Math.round(choice.row.preview.thickness * 1000)}</small>
              {:else if choice.row.number !== undefined}
                <small>{choice.row.number}</small>
              {/if}
            </button>
          {/each}
        {/each}
      </div>
    </div>
  {/if}
</div>

<style>
  .attribute-picker { position: relative; }
  .field { display: flex; width: 100%; height: 19px; align-items: center; padding: 0 3px 0 4px; border: 1px solid transparent; border-radius: 2px; background: var(--canvas); color: var(--text); font: 9px/1.2 'Segoe UI', sans-serif; gap: 5px; text-align: left; }
  .field:hover:not(:disabled) { border-color: var(--border); }
  .field[aria-expanded='true'] { border-color: var(--node-color); }
  .field:disabled { color: var(--text-faint); }
  .field.unknown { color: var(--danger); }
  .name { overflow: hidden; flex: 1; text-overflow: ellipsis; white-space: nowrap; }
  .caret { color: var(--text-faint); font-size: 9px; }

  /*
   * Above every node and its panels. The picker is opened from a row that is
   * itself inside a node, so anything less puts the list behind the next node
   * along - and a list you cannot see all of is not a list.
   */
  .popover { position: absolute; z-index: 30; top: calc(100% + 3px); left: 0; display: grid; width: max(230px, 100%); max-height: 260px; grid-template-rows: auto minmax(0, 1fr); overflow: hidden; border: 1px solid var(--border); border-radius: 3px; background: var(--surface-raised); box-shadow: 0 14px 34px rgb(0 0 0 / 45%); }
  .popover header { display: flex; align-items: center; padding: 5px; border-bottom: 1px solid var(--border); gap: 3px; }
  .search { display: flex; flex: 1; align-items: center; padding: 0 5px; border: 1px solid var(--border); border-radius: 2px; background: var(--canvas); gap: 4px; }
  .search span { color: var(--text-faint); font-size: 10px; }
  .search input { width: 100%; min-width: 0; height: 19px; padding: 0; border: 0; background: transparent; color: var(--text); font: 10px/1.2 'Segoe UI', sans-serif; }
  .search input:focus { outline: none; }
  .popover header > button { width: 21px; height: 21px; padding: 0; border-color: transparent; background: transparent; color: var(--text-faint); font-size: 11px; }
  .popover header > button.active { border-color: var(--border); background: var(--surface-hover); color: var(--text); }

  .rows { overflow-y: auto; padding: 3px; }
  h4 { position: sticky; top: 0; margin: 4px 0 2px; padding: 3px 5px; background: var(--surface-raised); color: var(--text-faint); font: 700 7px/1 ui-monospace, monospace; letter-spacing: .1em; text-transform: uppercase; }
  .row { display: flex; width: 100%; height: auto; min-height: 21px; align-items: center; padding: 2px 5px; border: 0; border-radius: 2px; background: transparent; color: var(--text); font: 10px/1.3 'Segoe UI', sans-serif; gap: 6px; text-align: left; }
  /* Hover and keyboard share one highlight: two different "where am I" marks on
     one list is one too many. */
  .row.active { background: var(--surface-hover); }
  .row.selected { color: var(--accent-soft); }
  .row.dimmed .label { opacity: .55; }
  .label { overflow: hidden; flex: 1; text-overflow: ellipsis; white-space: nowrap; }
  .row em { color: var(--text-faint); font: 7px/1 ui-monospace, monospace; font-style: normal; }
  .row small { color: var(--text-faint); font: 8px/1 ui-monospace, monospace; }
  .empty { margin: 0; padding: 14px 8px; color: var(--text-faint); font-size: 10px; text-align: center; }
</style>
