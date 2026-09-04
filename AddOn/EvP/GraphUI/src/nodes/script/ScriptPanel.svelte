<script lang="ts">
  import {
    EMPTY_SCRIPT_STATUS,
    SCRIPT_POLL_INTERVAL_MS,
    canRevealScript,
    conditionOf,
    summaryOf,
    workspaceNameOf,
    type ScriptStatus,
  } from './script'
  import { createScript, fetchScriptStatus, openScriptInEditor, reloadScript, revealScriptInExplorer } from './scriptBridge'
  import { FOLDER } from './icons'

  let {
    nodeId,
    graphId,
    path,
    onpathchange,
    onreloaded,
    onedit,
  }: {
    nodeId: string
    graphId?: string
    /**
     * The node's `scriptPath` parameter, as the document holds it.
     *
     * A FOLDER now, not a file: `main.py` inside it is the entry point. A bare
     * name resolves inside the workflow library, which is what lets a saved graph
     * name a node without an absolute path that would be wrong on every other
     * machine.
     */
    path: string
    /** Writes the path back through the ordinary parameter edit. */
    onpathchange: (path: string) => void
    /**
     * A reload landed and may have reshaped the node. The editor refetches the
     * graph state, because the node's ports live there and not here.
     */
    onreloaded: (status: ScriptStatus) => void
    /**
     * Open this node's file in the Script Inspector.
     *
     * ⚠️ THE EDITOR IS NOT DRAWN ON THE NODE, AND IT SHOULD NOT BE. A node body
     * is a few hundred pixels wide at a readable zoom, is re-rendered as the
     * canvas moves, and disappears when the node is scrolled off - none of which
     * a text buffer someone is typing into can survive. The panel asks for the
     * inspector; the inspector lives at the canvas level, where it can be pinned,
     * sized, and left open while the user clicks around the graph.
     */
    onedit?: () => void
  } = $props()

  let status = $state<ScriptStatus>(EMPTY_SCRIPT_STATUS)
  let busy = $state(false)
  let actionError = $state('')
  let now = $state(Date.now())
  let logOpen = $state(false)

  /*
   * ⚠️ THE FIELD IS RESYNCED ONLY WHEN THE DOCUMENT'S PATH ACTUALLY CHANGES, not
   * on every render. Binding the input straight to the prop would rewrite what the
   * user is halfway through typing every time a poll came back - a path is long,
   * and losing it at character forty is the kind of thing that makes someone stop
   * using a feature.
   *
   * Both start empty and are filled by the effect on its first run rather than
   * being initialised from `path`: reading a prop into $state captures only its
   * initial value, which is a real bug on a node whose path arrives a moment after
   * the component mounts - and it is what svelte-check warns about.
   */
  let draftPath = $state('')
  let lastKnownPath = $state('')
  $effect(() => {
    if (path !== lastKnownPath) {
      lastKnownPath = path
      draftPath = path
    }
  })

  const condition = $derived(conditionOf(status))
  const summary = $derived(summaryOf(status, now))
  // The FOLDER's name, not the entry file's: every node's entry is called
  // main.py, so showing that would label every script node identically.
  const fileName = $derived(workspaceNameOf(status))
  const helperCount = $derived(status.files.filter((file) => !file.shared && !file.entry).length)

  async function refresh(): Promise<void> {
    try {
      status = await fetchScriptStatus(nodeId, graphId)
    } catch {
      /*
       * Swallowed on purpose, and ONLY here. This runs on a timer; a bridge that
       * is momentarily unavailable would otherwise paint an error over a node
       * that is perfectly fine, once every second and a half. Every action the
       * user actually pressed reports its failure - see below.
       */
    }
  }

  async function act(work: () => Promise<ScriptStatus>): Promise<void> {
    busy = true
    actionError = ''
    try {
      status = await work()
      onreloaded(status)
    } catch (error) {
      actionError = error instanceof Error ? error.message : String(error)
    } finally {
      busy = false
    }
  }

  function commitPath(): void {
    const trimmed = draftPath.trim()
    if (trimmed === path) return
    onpathchange(trimmed)
  }

  $effect(() => {
    void refresh()
    /*
     * Polling is the FALLBACK for a missed or absent watcher - see the note on
     * SCRIPT_POLL_INTERVAL_MS. It stops while the window is hidden: a palette
     * behind Archicad's model window has nobody looking at it, and a timer
     * running there is bridge traffic in exchange for nothing.
     */
    const timer = window.setInterval(() => {
      now = Date.now()
      if (!document.hidden) void refresh()
    }, SCRIPT_POLL_INTERVAL_MS)
    return () => window.clearInterval(timer)
  })

  /*
   * A stale file reloads ITSELF when the native side is watching. The watcher has
   * already reloaded it by the time this is seen, so what this covers is the gap
   * where the notification was missed - and it is guarded on `busy` so a reload
   * in flight is never started twice.
   */
  $effect(() => {
    if (status.stale && status.watching && !busy) void act(() => reloadScript(nodeId, graphId))
  })

  async function create(): Promise<void> {
    const target = draftPath.trim()
    if (target === '') {
      actionError = 'Name the folder first'
      return
    }
    await act(() => createScript(nodeId, target, graphId))
  }

  /**
   * The two shell actions. Neither returns a status, so unlike `act` they do not
   * touch `status` - a failure here says so and changes nothing else, because
   * failing to OPEN a file has no bearing on what the node loaded.
   */
  async function shell(work: () => Promise<void>): Promise<void> {
    busy = true
    actionError = ''
    try {
      await work()
    } catch (error) {
      actionError = error instanceof Error ? error.message : String(error)
    } finally {
      busy = false
    }
  }
</script>

<section class="script nodrag nowheel">
  <!--
    The path is a plain text field, not a folder picker. A browser inside Archicad
    has no trustworthy way to open a native folder dialog, and a bare name is
    usually all that is wanted anyway: it resolves inside the workflow library at
    %LOCALAPPDATA%\Tapioca\Commands\Workflows, which is the folder
    scripts\Sync-All.ps1 deploys to. An absolute path still works, for a node
    that lives outside the library.
  -->
  <label class="path">
    <span>Folder</span>
    <input
      type="text"
      spellcheck="false"
      placeholder="apartment_metrics"
      bind:value={draftPath}
      onblur={commitPath}
      onkeydown={(event) => { if (event.key === 'Enter') commitPath() }}
    />
  </label>

  <div class="status" class:missing={condition === 'missing'} class:invalid={condition === 'invalid'} class:stale={condition === 'stale'}>
    <strong>{fileName === '' ? 'No folder' : fileName}</strong>
    <span>{summary}</span>
  </div>

  <!--
    Said on the node itself, because a helper is invisible from the canvas
    otherwise: a node whose behaviour lives half in calculations.py looks exactly
    like one that does not, right up until somebody edits the wrong file.
  -->
  {#if helperCount > 0}
    <p class="hint">{helperCount} helper file{helperCount === 1 ? '' : 's'} beside the main file.</p>
  {/if}

  <!--
    ⚠️ SAID OUT LOUD RATHER THAN IMPLIED. Without a watcher the node picks a save
    up on its next evaluation or on Reload - which is a perfectly workable way to
    work, and a miserable one to discover by wondering why nothing happened.
  -->
  {#if status.path !== '' && !status.watching}
    <p class="hint">Not watching this folder — press Reload after you save.</p>
  {/if}

  {#if status.diagnostics.length > 0}
    <ul class="diagnostics">
      {#each status.diagnostics as diagnostic}
        <li><span>{diagnostic.line > 0 ? `line ${diagnostic.line}` : 'file'}</span><code>{diagnostic.message}</code></li>
      {/each}
    </ul>
  {/if}

  <!--
    Reloading is the one action that can remove wires the user did not touch, so
    when it does it says which. Left silent, a renamed argument would look like
    the canvas had quietly eaten a connection.
  -->
  {#if status.droppedEdges.length > 0}
    <p class="dropped">
      {status.droppedEdges.length} connection{status.droppedEdges.length === 1 ? '' : 's'} dropped: the port
      {status.droppedEdges.length === 1 ? 'it used' : 'they used'} no longer exists or changed type.
    </p>
  {/if}

  {#if actionError !== ''}<p class="error">{actionError}</p>{/if}

  <div class="actions">
    <button type="button" disabled={busy} onclick={() => act(() => reloadScript(nodeId, graphId))}>
      {busy ? 'Working…' : 'Reload'}
    </button>
    {#if condition === 'empty' || condition === 'missing'}
      <button type="button" disabled={busy} onclick={create}>Create</button>
    {:else}
      <!--
        Edit here, Open there, and both are worth a button. "Edit" is the
        ten-second fix without leaving the canvas; "Open" is the rest of the work,
        in the editor the user already has the file open in. Neither replaces the
        other, and offering only one of them would be picking for them.
      -->
      <button type="button" disabled={busy || onedit === undefined} onclick={() => onedit?.()}>Edit</button>
      <button type="button" disabled={busy} onclick={() => shell(() => openScriptInEditor(nodeId, graphId))}>Open</button>
    {/if}
    <!--
      Show the file in Explorer. Icon-only because it is the one action here whose
      meaning a folder draws better than any word this narrow button could hold -
      "Folder", "Show", "Reveal" each read as something slightly different. It
      still carries a title and an aria-label, so it is not an icon-only control
      to a screen reader or to anyone hovering.

      Enabled only once there is a file: revealing a path that does not exist can
      only produce an error, and an action that is offered and then refused is
      worse than one that was never offered.
    -->
    <button
      type="button"
      class="icon"
      disabled={busy || !canRevealScript(status)}
      title={canRevealScript(status) ? `Show ${fileName} in Explorer` : 'No file to show yet'}
      aria-label="Show the script in Explorer"
      onclick={() => shell(() => revealScriptInExplorer(nodeId, graphId))}
    >
      <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke-width="1.6" aria-hidden="true">
        {#each FOLDER.paths as path}<path d={path} stroke="currentColor" stroke-linecap="round" stroke-linejoin="round" />{/each}
      </svg>
    </button>
    {#if status.log.length > 0}
      <button type="button" class="toggle" onclick={() => (logOpen = !logOpen)}>
        Output ({status.log.length})
      </button>
    {/if}
  </div>

  {#if logOpen && status.log.length > 0}
    <!--
      Whatever the script printed. Not a nicety: a script node runs on a worker
      thread inside Archicad with no console attached, so without somewhere to
      show this, `print` goes nowhere and the most common way anyone debugs a
      script silently does not work.
    -->
    <ol class="log">{#each status.log as line}<li><code>{line}</code></li>{/each}</ol>
  {/if}
</section>

<style>
  .script { display: grid; padding: 0 9px 9px 11px; gap: 6px; }
  .path { display: grid; grid-template-columns: 26px 1fr; align-items: center; gap: 6px; }
  .path span { color: var(--text-faint); font-size: 8px; }
  .path input { min-width: 0; height: 20px; padding: 0 5px; border: 1px solid var(--border); border-radius: 3px; background: var(--canvas); color: var(--text); font: 9px/1 ui-monospace, monospace; }
  .status { display: flex; justify-content: space-between; align-items: baseline; gap: 8px; }
  .status strong { overflow: hidden; color: var(--text); font: 600 9px/1.3 'Segoe UI', sans-serif; text-overflow: ellipsis; white-space: nowrap; }
  .status span { flex: 0 0 auto; color: var(--text-faint); font-size: 8px; }
  /* The three conditions are coloured, and only they are: a node that is simply
     loaded should look like every other node on the canvas. */
  .status.missing span, .error { color: #d05f5f; }
  .status.invalid span { color: #d98f3f; }
  .status.stale span { color: var(--accent, #4c8cd8); }
  .hint { margin: 0; color: var(--text-faint); font-size: 8px; }
  .diagnostics { margin: 0; padding: 0; list-style: none; display: grid; gap: 2px; }
  .diagnostics li { display: grid; grid-template-columns: 44px 1fr; align-items: baseline; gap: 5px; }
  .diagnostics span { color: var(--text-faint); font: 8px/1.4 ui-monospace, monospace; }
  .diagnostics code { color: #d98f3f; font: 9px/1.4 ui-monospace, monospace; white-space: pre-wrap; }
  .dropped { margin: 0; color: #d98f3f; font-size: 8px; }
  .error { margin: 0; font-size: 8px; }
  .actions { display: flex; gap: 4px; }
  /* The icon button does not stretch: the text buttons share the row and this one
     is exactly as wide as its glyph needs. */
  .actions button.icon { flex: 0 0 24px; display: grid; place-items: center; padding: 0; }
  .actions button { flex: 1 1 0; height: 21px; border: 1px solid var(--border); border-radius: 3px; background: var(--surface); color: var(--text); font: 600 9px/1 'Segoe UI', sans-serif; cursor: pointer; }
  .actions button:hover:not(:disabled) { border-color: var(--accent, #4c8cd8); }
  .actions button:disabled { color: var(--text-faint); cursor: default; }
  .log { max-height: 120px; margin: 0; padding: 4px 0; overflow: auto; border: 1px solid var(--border); background: var(--canvas); list-style: none; }
  .log li { padding: 1px 7px; }
  .log code { color: var(--text); font: 9px/1.4 ui-monospace, monospace; white-space: pre-wrap; }
</style>
