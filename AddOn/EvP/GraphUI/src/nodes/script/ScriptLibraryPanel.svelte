<script lang="ts">
  /**
   * The script picker: what is in the workflow library, and which one this node runs.
   *
   * ⚠️ THIS IS WHAT A SCRIPT NODE'S POP-OUT IS FOR, AND IT REPLACED THE GENERIC
   * ONE. Every other node's pop-out browses the DATA it produced, because that is
   * the interesting thing about a Number or a Selection. A script node's
   * interesting fact is which script it is - the question asked of it a dozen
   * times an hour is "run the other one", and answering that used to mean knowing
   * the folder's name by heart and typing it into a field. The library is right
   * there on disk; listing it is the whole feature.
   *
   * ⚠️ AND CHOOSING ONE IS AN ORDINARY PARAMETER EDIT. It writes `scriptPath`
   * through the same path the text field does, so it is validated, revisioned and
   * undoable like anything else - and a bare folder name means the graph stays
   * portable to a machine whose LOCALAPPDATA is somewhere else.
   */
  import { closeOnOutsidePress } from '../../interaction'
  import { describeAge } from './script'
  import { fetchScriptLibrary, openScriptLibraryFolder, type ScriptLibraryEntry } from './scriptBridge'

  let {
    nodeId,
    graphId,
    current,
    onchoose,
    onclose,
  }: {
    nodeId: string
    graphId?: string
    /** The node's `scriptPath`, so the row it is already on is marked. */
    current: string
    onchoose: (name: string) => void
    onclose: () => void
  } = $props()

  let entries = $state<ScriptLibraryEntry[]>([])
  let root = $state('')
  let error = $state('')
  let loaded = $state(false)
  let filter = $state('')
  let now = $state(Date.now())

  const matches = $derived.by(() => {
    const needle = filter.trim().toLowerCase()
    if (needle === '') return entries
    return entries.filter(
      (entry) => entry.name.toLowerCase().includes(needle) || entry.title.toLowerCase().includes(needle),
    )
  })

  /*
   * Read once, when the panel opens, and again only when the user asks.
   *
   * ⚠️ DELIBERATELY NOT POLLED. Every script node on the canvas already polls its
   * own status once a second and a half; a picker that also enumerated the whole
   * library on a timer would put a directory scan behind a panel that is open for
   * four seconds at a time. A script added in Explorer while this is open is what
   * the Refresh row is for.
   */
  async function load(): Promise<void> {
    try {
      const library = await fetchScriptLibrary(nodeId, graphId)
      entries = library.entries
      root = library.root
      error = ''
    } catch (failure) {
      error = failure instanceof Error ? failure.message : String(failure)
    } finally {
      loaded = true
      now = Date.now()
    }
  }

  $effect(() => {
    void load()
  })

  async function reveal(): Promise<void> {
    try {
      await openScriptLibraryFolder()
    } catch (failure) {
      error = failure instanceof Error ? failure.message : String(failure)
    }
  }

  function choose(entry: ScriptLibraryEntry): void {
    if (!entry.hasEntry) return
    onchoose(entry.name)
    onclose()
  }
</script>

<aside class="library nodrag nowheel" aria-label="Scripts in the workflow library" use:closeOnOutsidePress={onclose}>
  <header>
    <div>
      <span>Workflow library</span>
      <strong title={root}>{entries.length} script{entries.length === 1 ? '' : 's'}</strong>
    </div>
    <button class="close" onclick={onclose} aria-label="Close the script picker">Close</button>
  </header>

  <label class="filter">
    <!-- svelte-ignore a11y_autofocus -->
    <input autofocus type="text" spellcheck="false" placeholder="Filter" bind:value={filter} />
  </label>

  {#if error !== ''}
    <p class="error">{error}</p>
  {:else if !loaded}
    <p class="note">Reading the library…</p>
  {:else if entries.length === 0}
    <!--
      An empty library is the ordinary state of a machine that has never had a
      workflow deployed, so it says where the folder is and offers to open it
      rather than reading as a failure.
    -->
    <p class="note">
      Nothing in the library yet. Press <strong>+</strong> on a node to write one, or put a folder in
      <code>{root === '' ? '%LOCALAPPDATA%\\Tapioca\\Commands\\Workflows' : root}</code>.
    </p>
  {:else}
    <ul>
      {#each matches as entry (entry.name)}
        <li>
          <button
            class="row"
            class:active={entry.name === current}
            disabled={!entry.hasEntry}
            title={entry.hasEntry ? `Run ${entry.name} on this node` : `${entry.name} has no main file, so no node can run it`}
            onclick={() => choose(entry)}
          >
            <span class="name">{entry.name}</span>
            <!-- The @name beside the folder, never instead of it: the folder is
                 what gets written into the graph, and two folders may carry the
                 same friendly title. -->
            {#if entry.title !== ''}<span class="title">{entry.title}</span>{/if}
            <span class="meta">
              {#if !entry.hasEntry}no main file{:else}{entry.fileCount} file{entry.fileCount === 1 ? '' : 's'} · {describeAge(entry.modifiedAtMs, now)}{/if}
            </span>
          </button>
        </li>
      {/each}
      {#if matches.length === 0}<li><p class="note">Nothing matches “{filter}”.</p></li>{/if}
    </ul>
  {/if}

  <footer>
    <button onclick={() => void load()}>Refresh</button>
    <button onclick={() => void reveal()} title={root}>Open the library folder</button>
  </footer>
</aside>

<style>
  /* Positioned against MasterNode's panel anchor, which is the node's own box -
     so `left: calc(100% + 10px)` puts this beside the node rather than over it. */
  .library { position: absolute; z-index: 15; top: 0; left: calc(100% + 10px); display: flex; width: 260px; max-height: 340px; flex-direction: column; overflow: hidden; border: 1px solid var(--border); border-radius: 5px; background: var(--surface); box-shadow: 0 14px 44px rgb(0 0 0 / 42%); color: var(--text); font: 11px/1.45 'Segoe UI', sans-serif; }
  header { display: flex; align-items: center; justify-content: space-between; padding: 8px 10px; border-bottom: 1px solid var(--border); }
  header div { display: grid; min-width: 0; }
  header span { color: var(--text-faint); font-size: 8px; letter-spacing: .08em; text-transform: uppercase; }
  header strong { font-size: 11px; }
  .filter { display: block; padding: 7px 10px 0; }
  .filter input { width: 100%; height: 21px; padding: 0 6px; border: 1px solid var(--border); border-radius: 3px; background: var(--canvas); color: var(--text); font: 10px/1 'Segoe UI', sans-serif; }
  ul { margin: 0; padding: 7px 0; overflow: auto; list-style: none; }
  .row { display: grid; width: 100%; padding: 4px 10px; border: 0; background: none; color: inherit; cursor: pointer; grid-template-columns: 1fr auto; text-align: left; }
  .row:hover:not(:disabled) { background: color-mix(in srgb, var(--text) 8%, transparent); }
  .row:disabled { cursor: default; opacity: .5; }
  /* The row the node is already on. Marked rather than hidden: seeing which one
     is current is most of what the picker is being opened to find out. */
  .row.active { background: color-mix(in srgb, var(--accent) 16%, transparent); }
  .row .name { overflow: hidden; font: 10px/1.4 ui-monospace, monospace; text-overflow: ellipsis; white-space: nowrap; }
  .row .title { grid-column: 1; color: var(--text-faint); font-size: 9px; }
  .row .meta { grid-row: 1 / span 2; align-self: center; color: var(--text-faint); font-size: 8px; white-space: nowrap; }
  .note, .error { margin: 0; padding: 12px 10px; font-size: 10px; }
  .note { color: var(--text-faint); }
  .note code { font: 9px ui-monospace, monospace; overflow-wrap: anywhere; }
  .error { color: var(--danger); }
  footer { display: flex; padding: 7px 10px; border-top: 1px solid var(--border); gap: 5px; }
  button { height: 21px; padding: 0 8px; border: 1px solid var(--border); border-radius: 3px; background: var(--canvas); color: var(--text); font: 600 9px/1 'Segoe UI', sans-serif; cursor: pointer; }
  button:hover:not(:disabled) { border-color: var(--accent); }
  footer button { flex: 1 1 0; }
</style>
