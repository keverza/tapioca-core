<script lang="ts">
  /**
   * THE OBJECT LIBRARY BROWSER, ported from the palette's modal.
   *
   * ⚠️ THREE PANES, AND THE SPLIT IS THE WHOLE POINT. Search across the top, the
   * FOLDER TREE on the left, the selected folder's CONTENTS on the right.
   * Palette/CatalogBrowser.hpp records why: its first cut put folders and objects
   * in one tree, and a user reported it unusable beside Archicad's own dialog -
   * every object sat inside a folder of its own, and there was nowhere for a
   * preview to go. The left pane stays short enough to scan because it holds only
   * folders; the right pane only ever shows one folder's worth.
   *
   * ⚠️ IT SHOWS ONLY WHAT CAN BE PLACED. That narrowing is the runtime's, not
   * this component's - Tapioca.ListLibraryParts with no subtype means GDL
   * objects, the same list Object Settings shows. Nothing here filters by type,
   * because a browser deciding what counts as placeable would be a second opinion
   * on a question the library already answers.
   *
   * ⚠️ AND A MISSING THUMBNAIL IS THE COMMON CASE. A stock library ships parts
   * with PNG previews, parts with TIFF previews no browser draws, and parts with
   * no preview section at all - all three were found in the first six objects of
   * the stock library. So the grid has a real name-only cell; it is not an error
   * state.
   */
  import type { LibraryCatalog, LibraryPartPreview, LibraryPartRow } from '../../types'
  import {
    ancestorKeys,
    buildFolderTree,
    cellSize,
    filterParts,
    folderKey,
    LIBRARY_ROOT_KEY,
    LIBRARY_VIEW_MODES,
    partsInFolder,
    type LibraryFolder,
    type LibraryViewMode,
  } from './libraryParts'

  let {
    catalog,
    selectedName = '',
    onchoose,
    onclose,
    onrequest,
    onpreview,
  }: {
    catalog?: LibraryCatalog
    selectedName?: string
    onchoose: (part: LibraryPartRow) => void
    onclose: () => void
    onrequest?: () => void
    onpreview?: (name: string) => Promise<LibraryPartPreview>
  } = $props()

  let query = $state('')
  let folder = $state(LIBRARY_ROOT_KEY)
  let expanded = $state(new Set<string>())
  let view = $state<LibraryViewMode>('medium')
  let highlighted = $state<LibraryPartRow | undefined>(undefined)

  // Fetched thumbnails, keyed by part name. A part whose preview came back
  // unusable is stored as an EMPTY string rather than left absent, so it is asked
  // for once and not on every repaint.
  const previews = $state(new Map<string, string>())
  const pending = new Set<string>()

  $effect(() => {
    if (catalog === undefined) onrequest?.()
  })

  const parts = $derived(catalog?.parts ?? [])
  const matching = $derived(filterParts(parts, query))
  const tree = $derived(buildFolderTree(matching))
  const contents = $derived(partsInFolder(matching, folder))

  // A search re-roots the contents pane: the folder you had open may no longer
  // contain anything that matches, and leaving it selected shows an empty pane
  // beside a tree full of results.
  $effect(() => {
    void query
    folder = LIBRARY_ROOT_KEY
  })

  function flatten(folders: LibraryFolder[]): LibraryFolder[] {
    const rows: LibraryFolder[] = []
    const walk = (list: LibraryFolder[]): void => {
      for (const entry of list) {
        rows.push(entry)
        if (expanded.has(entry.key)) walk(entry.children)
      }
    }
    walk(folders)
    return rows
  }

  const visibleFolders = $derived(flatten(tree))

  function toggle(key: string): void {
    const next = new Set(expanded)
    if (next.has(key)) next.delete(key)
    else next.add(key)
    expanded = next
  }

  function reveal(part: LibraryPartRow): void {
    const path = part.treePath.length > 0 ? part.treePath : ['Loaded Libraries']
    const next = new Set(expanded)
    for (const key of ancestorKeys(path)) next.add(key)
    next.add(folderKey(path))
    expanded = next
    folder = folderKey(path)
  }

  /**
   * How many thumbnails one folder is allowed to fetch.
   *
   * ⚠️ A BUDGET, NOT A CONVENIENCE. Every preview is a separate bridge call that
   * touches the library file on Archicad's main thread; "All objects" on a stock
   * library is several thousand of them, and firing that on a folder click would
   * lock the host for a grid nobody has scrolled to. Cells past the budget draw
   * as name-only, and the footer says so - which is the same honesty the
   * missing-preview cell already carries.
   */
  const PREVIEW_BUDGET = 120

  async function loadPreview(name: string): Promise<void> {
    if (onpreview === undefined || previews.has(name) || pending.has(name)) return
    pending.add(name)
    try {
      const answer = await onpreview(name)
      previews.set(name, answer.dataUri)
    } catch {
      // A failed fetch caches as "no picture" like any other. Retrying on every
      // repaint would hammer the bridge for a thumbnail.
      previews.set(name, '')
    } finally {
      pending.delete(name)
    }
  }

  const budgeted = $derived(view === 'list' ? [] : contents.slice(0, PREVIEW_BUDGET))
  const overBudget = $derived(view === 'list' ? 0 : Math.max(0, contents.length - PREVIEW_BUDGET))

  $effect(() => {
    // The list view asks for nothing at all: it draws no pictures, so fetching
    // them would be paying the main thread for something nobody sees.
    for (const part of budgeted) void loadPreview(part.name)
  })

  function cellClass(part: LibraryPartRow): string {
    return part.name === selectedName ? 'cell chosen' : 'cell'
  }
</script>

<div class="browser nodrag nowheel">
  <header>
    <input
      class="search"
      bind:value={query}
      placeholder="Search objects and folders"
      aria-label="Search the object library"
    />
    <div class="views" role="group" aria-label="View">
      {#each LIBRARY_VIEW_MODES as entry (entry.mode)}
        <button type="button" class:active={view === entry.mode} onclick={() => (view = entry.mode)}>{entry.label}</button>
      {/each}
    </div>
    <button type="button" class="close" onclick={onclose}>Close</button>
  </header>

  {#if catalog === undefined}
    <p class="note">Reading the loaded libraries...</p>
  {:else if catalog.error !== undefined}
    <p class="note failure">{catalog.error}</p>
  {:else if parts.length === 0}
    <!-- Not an error: a project with no loaded object library is a real state,
         and it needs a sentence rather than an empty tree that reads as broken. -->
    <p class="note">This project has no placeable library objects loaded.</p>
  {:else}
    <div class="panes">
      <nav aria-label="Folders">
        <button type="button" class:active={folder === LIBRARY_ROOT_KEY} onclick={() => (folder = LIBRARY_ROOT_KEY)}>
          <span class="name">All objects</span>
          <span class="count">{matching.length}</span>
        </button>
        {#each visibleFolders as entry (entry.key)}
          <div class="folder-row" style={`--depth: ${entry.depth}`}>
            <button
              type="button"
              class="twist"
              disabled={entry.children.length === 0}
              aria-label={expanded.has(entry.key) ? `Collapse ${entry.label}` : `Expand ${entry.label}`}
              onclick={() => toggle(entry.key)}
            >{entry.children.length === 0 ? '' : expanded.has(entry.key) ? '▾' : '▸'}</button>
            <button type="button" class="folder" class:active={folder === entry.key} onclick={() => (folder = entry.key)}>
              <span class="name">{entry.label}</span>
              <!-- The SUBTREE total, so an intermediate level that holds nothing
                   itself still says how much is underneath it. -->
              <span class="count">{entry.totalCount}</span>
            </button>
          </div>
        {/each}
      </nav>

      <section class="contents" class:list={view === 'list'} style={`--cell: ${cellSize(view)}px`}>
        {#each contents as part, index (part.unID + part.name)}
          <button
            type="button"
            class={cellClass(part)}
            title={`${part.name}\n${part.file}${part.location === '' ? '' : `\n${part.location}`}`}
            onclick={() => (highlighted = part)}
            ondblclick={() => onchoose(part)}
          >
            {#if view !== 'list'}
              {@const cached = previews.get(part.name)}
              <span class="thumb">
                {#if cached === undefined}
                  <em>{index < 120 ? '...' : 'no preview'}</em>
                {:else if cached === ''}
                  <!-- The name-only cell. A large fraction of a stock library
                       lands here - TIFF previews, and parts with no preview
                       section at all - so it is a design, not a fallback. -->
                  <em>no preview</em>
                {:else}
                  <img src={cached} alt="" loading="lazy" />
                {/if}
              </span>
            {/if}
            <span class="label">{part.name}</span>
            {#if part.missing}<span class="missing">missing</span>{/if}
          </button>
        {:else}
          <p class="note">
            {query.trim() === ''
              ? 'This folder holds no objects of its own - open one of its subfolders.'
              : `Nothing here matches "${query}".`}
          </p>
        {/each}
      </section>
    </div>

    <footer>
      <div class="status">
        {#if highlighted !== undefined}
          <strong>{highlighted.name}</strong>
          <span>{highlighted.type}{highlighted.embedded ? ' / Embedded' : highlighted.library === '' ? '' : ` / ${highlighted.library}`}</span>
          <button type="button" class="link" onclick={() => reveal(highlighted!)}>Show folder</button>
        {:else}
          <span>{matching.length} of {catalog.total} object{catalog.total === 1 ? '' : 's'}</span>
          {#if catalog.truncated}
            <!-- SAID, not absorbed: a silently shortened list reads as "the
                 library does not have it". -->
            <span class="capped">the runtime capped this listing</span>
          {/if}
          {#if overBudget > 0}
            <span class="capped">{overBudget} cell{overBudget === 1 ? '' : 's'} without thumbnails</span>
          {/if}
        {/if}
      </div>
      <button type="button" class="choose" disabled={highlighted === undefined} onclick={() => highlighted && onchoose(highlighted)}>
        Use this object
      </button>
    </footer>
  {/if}
</div>

<style>
  .browser {
    position: absolute;
    z-index: 14;
    top: 0;
    left: calc(100% + 10px);
    display: grid;
    width: 420px;
    max-height: 440px;
    grid-template-rows: auto minmax(0, 1fr) auto;
    overflow: hidden;
    border: 1px solid var(--border);
    border-top: 2px solid var(--node-color);
    border-radius: 2px;
    background: var(--surface);
    box-shadow: 0 18px 44px rgb(0 0 0 / 48%);
    color: var(--text);
  }
  header { display: flex; align-items: center; padding: 8px; border-bottom: 1px solid var(--border); background: var(--surface-raised); gap: 6px; }
  .search { flex: 1 1 auto; min-width: 0; height: 24px; padding: 0 6px; }
  .views { display: flex; gap: 2px; }
  .views button, .close { height: 24px; padding: 0 7px; font-size: 9px; }
  .views button.active { border-color: var(--node-color); color: var(--text); }
  .panes { display: grid; min-height: 0; grid-template-columns: 168px minmax(0, 1fr); }
  nav { min-height: 0; padding: 4px; overflow: auto; border-right: 1px solid var(--border); background: var(--canvas); }
  nav > button, .folder { display: flex; width: 100%; height: 20px; align-items: center; padding: 0 5px; border: 0; border-radius: 2px; background: transparent; color: var(--text); cursor: pointer; gap: 5px; }
  .folder-row { display: flex; align-items: center; padding-left: calc(var(--depth) * 10px); }
  .twist { width: 13px; height: 20px; flex: 0 0 13px; padding: 0; border: 0; background: transparent; color: var(--text-faint); font-size: 8px; cursor: pointer; }
  .twist:disabled { cursor: default; }
  .name { overflow: hidden; flex: 1 1 auto; font: 9px/1 'Segoe UI', sans-serif; text-align: left; text-overflow: ellipsis; white-space: nowrap; }
  .count { color: var(--text-faint); font: 8px/1 ui-monospace, monospace; }
  nav > button:hover, .folder:hover { background: var(--surface-raised); }
  nav > button.active, .folder.active { background: var(--surface-raised); color: var(--text); box-shadow: inset 2px 0 0 var(--node-color); }
  .contents { display: grid; min-height: 0; padding: 6px; overflow: auto; align-content: start; gap: 5px; grid-template-columns: repeat(auto-fill, minmax(var(--cell), 1fr)); }
  .contents.list { display: block; }
  .cell { display: grid; width: 100%; padding: 4px; border: 1px solid transparent; border-radius: 2px; background: transparent; color: var(--text); cursor: pointer; gap: 3px; justify-items: center; }
  .contents.list .cell { display: flex; height: 20px; align-items: center; padding: 0 5px; }
  .cell:hover { background: var(--surface-raised); }
  .cell.chosen { border-color: var(--node-color); background: var(--surface-raised); }
  .thumb { display: grid; width: 100%; height: var(--cell); align-items: center; justify-items: center; overflow: hidden; background: var(--canvas); }
  .thumb img { max-width: 100%; max-height: 100%; }
  .thumb em { color: var(--text-faint); font: 8px/1.2 'Segoe UI', sans-serif; font-style: normal; text-align: center; }
  .label { overflow: hidden; width: 100%; font: 9px/1.25 'Segoe UI', sans-serif; text-align: center; text-overflow: ellipsis; white-space: nowrap; }
  .contents.list .label { text-align: left; }
  .missing { color: var(--danger, #d8746c); font: 8px/1 'Segoe UI', sans-serif; }
  footer { display: flex; align-items: center; padding: 7px 8px; border-top: 1px solid var(--border); background: var(--surface-raised); gap: 8px; }
  .status { display: flex; min-width: 0; flex: 1 1 auto; align-items: baseline; gap: 6px; }
  .status strong { overflow: hidden; font-size: 10px; font-weight: 550; text-overflow: ellipsis; white-space: nowrap; }
  .status span { color: var(--text-faint); font: 8px/1.3 'Segoe UI', sans-serif; }
  .capped { color: var(--warning, #d8b06c); }
  .link { border: 0; background: transparent; color: var(--text-faint); font-size: 8px; text-decoration: underline; cursor: pointer; }
  .choose { height: 24px; padding: 0 10px; border: 1px solid var(--border); border-radius: 3px; background: var(--accent, #4c8cd8); color: #fff; font: 600 9px/1 'Segoe UI', sans-serif; cursor: pointer; }
  .choose:disabled { background: var(--surface); color: var(--text-faint); cursor: default; }
  .note { margin: 0; padding: 18px 12px; color: var(--text-faint); font: 9px/1.45 'Segoe UI', sans-serif; text-align: center; }
  .failure { color: var(--danger, #d8746c); }
</style>
