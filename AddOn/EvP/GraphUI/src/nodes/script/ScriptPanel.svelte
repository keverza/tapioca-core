<script lang="ts">
  /**
   * A script node's body: what its folder is, and the four things you can do to it.
   *
   * ⚠️ THE ACTIONS ARE ICONS BECAUSE THE ROW IS 24 PIXELS OF A 220-PIXEL NODE.
   * Four worded buttons at that width are four truncated words; the meaning has
   * to come from the glyph, and each one still carries a title and an aria-label
   * so it is not icon-only to a screen reader or to anyone hovering. See icons.ts
   * for why each glyph is the one it is.
   *
   * ⚠️ AND NOTHING IS WRITTEN TO DISK UNTIL SOMEBODY ASKS FOR IT. A node placed a
   * moment ago has no folder: it is not broken, it is new, and it says "New
   * script" rather than an error. Create scaffolds one under a name the native
   * side picks; the Inspector scaffolds one on its first save. Placing a node was
   * never a request to put a folder in the user's library, and a canvas someone
   * has been experimenting on for an hour must not leave twenty of them behind.
   */
  import {
    EMPTY_SCRIPT_STATUS,
    SCRIPT_POLL_INTERVAL_MS,
    conditionOf,
    summaryOf,
    workspaceNameOf,
    type ScriptStatus,
  } from './script'
  import {
    createScript,
    fetchScriptLibrary,
    fetchScriptStatus,
    reloadScript,
  } from './scriptBridge'
  import { CODE_BRACKETS, FOLDER, PAGE_PLUS, RELOAD_WINDOW, type IconGlyph } from './icons'

  let {
    nodeId,
    graphId,
    path,
    compact = false,
    onpathchange,
    onreloaded,
    onedit,
    onbrowselibrary,
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
    /**
     * The minimal presentation, from the node's own view mode.
     *
     * ⚠️ IT STILL DRAWS THE CONDITION, AND THAT IS THE WHOLE ARGUMENT FOR THIS
     * MODE EXISTING RATHER THAN THE BODY SIMPLY BEING HIDDEN. A minimal node is
     * for a graph someone has finished building and wants to read; a script that
     * has since gone missing or stopped parsing is exactly what they need to see
     * on such a graph, and it is the one thing a bare header cannot tell them.
     */
    compact?: boolean
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
    /**
     * Open the script picker - the pop-out listing what is in the workflow
     * library. It is a PANEL rather than a dropdown here for the same reason the
     * inspector is: it is positioned against the node by MasterNode's panel
     * anchor, and a select element inside a node body is clipped by the node.
     */
    onbrowselibrary?: () => void
  } = $props()

  let status = $state<ScriptStatus>(EMPTY_SCRIPT_STATUS)
  let busy = $state(false)
  let actionError = $state('')
  let now = $state(Date.now())
  let logOpen = $state(false)
  /**
   * The name Create would use, from the native side.
   *
   * Fetched once, lazily, and only for a node that has no folder - it is the
   * placeholder in the name field, so the user can see what pressing Create is
   * about to make before they press it. Empty until it arrives, which is why
   * Create asks again rather than trusting this.
   */
  let suggestedName = $state('')

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
  /** A node with no folder is NEW, not broken. See the header note. */
  const unscaffolded = $derived(condition === 'empty')

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

  /* The suggested name, once, for a node that has none. Not polled: the answer
     only goes stale if another node claims that name, and Create re-asks. */
  $effect(() => {
    if (compact || !unscaffolded || suggestedName !== '') return
    void (async () => {
      try {
        suggestedName = (await fetchScriptLibrary(nodeId, graphId)).suggestedName
      } catch {
        /* The placeholder stays empty. Create still works - it asks again. */
      }
    })()
  })

  /**
   * Scaffold this node's folder.
   *
   * ⚠️ AN EMPTY NAME IS NOT AN ERROR ANY MORE, AND THAT IS THE FIX. Create used
   * to refuse until the user had typed a folder name into a field they had no
   * reason to know was required - a button that is offered and then refuses is
   * the least intuitive thing a panel can do. With nothing typed it asks the
   * native side for the next free name and uses that; a name that WAS typed is
   * still honoured, because naming the thing you are about to make is a
   * perfectly reasonable way to work.
   */
  async function create(): Promise<void> {
    busy = true
    actionError = ''
    try {
      let target = draftPath.trim()
      if (target === '') {
        // Asked again rather than reusing the placeholder: this is the moment a
        // folder is actually claimed, and the listing behind that placeholder may
        // be a minute old.
        target = (await fetchScriptLibrary(nodeId, graphId)).suggestedName
      }
      if (target === '') {
        actionError = 'No workflow library on this machine — type an absolute folder path'
        return
      }
      status = await createScript(nodeId, target, graphId)
      onreloaded(status)
    } catch (error) {
      actionError = error instanceof Error ? error.message : String(error)
    } finally {
      busy = false
    }
  }

  /** The four icon actions, in the order they are used. */
  const actions = $derived<
    { glyph: IconGlyph; label: string; title: string; disabled: boolean; run: () => void }[]
  >([
    {
      glyph: RELOAD_WINDOW,
      label: 'Reload this script',
      title: 'Read the folder again and reshape this node',
      disabled: busy || unscaffolded,
      run: () => void act(() => reloadScript(nodeId, graphId)),
    },
    {
      glyph: PAGE_PLUS,
      label: 'Create this script',
      title: unscaffolded
        ? `Write a starter script${suggestedName === '' ? '' : ` in ${suggestedName}`}`
        : 'This node already has a folder',
      disabled: busy || !unscaffolded,
      run: () => void create(),
    },
    {
      glyph: CODE_BRACKETS,
      label: 'Edit this script',
      title: 'Open the Script Inspector',
      disabled: busy || onedit === undefined,
      run: () => onedit?.(),
    },
    {
      glyph: FOLDER,
      label: 'Choose a script',
      title: 'Load a script from the workflow library',
      disabled: busy || onbrowselibrary === undefined,
      run: () => onbrowselibrary?.(),
    },
  ])
</script>

{#if compact}
  <!--
    Minimal: the node's own name and one coloured dot. Everything else on this
    panel is a control, and a minimal node is one nobody is currently operating -
    but a script that has gone missing or stopped parsing still has to be visible
    from across the canvas, because on a minimal node there is nothing else that
    could show it.
  -->
  <section class="minimal">
    <span class="dot" class:missing={condition === 'missing'} class:invalid={condition === 'invalid'} class:stale={condition === 'stale'} class:empty={unscaffolded} title={summary}></span>
    <span class="name">{fileName === '' ? 'New script' : fileName}</span>
  </section>
{:else}
<section class="script nodrag nowheel">
  <!--
    The path is a plain text field, not a folder picker. A browser inside Archicad
    has no trustworthy way to open a native folder dialog, and it is no longer the
    way anyone is expected to point a node at an existing script - the picker does
    that. What is left for this field is naming a NEW folder before creating it,
    and pointing at one outside the library by absolute path.
  -->
  <label class="path">
    <span>Folder</span>
    <input
      type="text"
      spellcheck="false"
      placeholder={unscaffolded ? suggestedName || 'new_script' : 'apartment_metrics'}
      bind:value={draftPath}
      onblur={commitPath}
      onkeydown={(event) => { if (event.key === 'Enter') commitPath() }}
    />
  </label>

  <div class="status" class:missing={condition === 'missing'} class:invalid={condition === 'invalid'} class:stale={condition === 'stale'}>
    <strong>{fileName === '' ? 'New script' : fileName}</strong>
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
    {#each actions as action (action.label)}
      <!--
        `data-menu-toggle` on the picker only: it is the one button here that
        opens a panel which closes on an outside press, and without the marker
        that press - this very one - would close the panel the click then
        reopens. See closeOnOutsidePress.
      -->
      <button type="button" class="icon" data-menu-toggle={action.glyph === FOLDER ? '' : undefined} disabled={action.disabled} title={action.title} aria-label={action.label} onclick={action.run}>
        <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke-width="1.6" aria-hidden="true">
          {#each action.glyph.paths as shape}<path d={shape} stroke="currentColor" stroke-linecap="round" stroke-linejoin="round" />{/each}
        </svg>
      </button>
    {/each}
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
{/if}

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
  .status.missing span, .error { color: var(--danger); }
  .status.invalid span { color: var(--warning); }
  .status.stale span { color: var(--accent); }
  /* Minimal: one row, no controls, and the same colour vocabulary as the status
     line above so the two modes cannot mean different things by the same hue. */
  .minimal { display: flex; align-items: center; padding: 0 9px 7px 11px; gap: 6px; }
  .minimal .dot { flex: 0 0 auto; width: 6px; height: 6px; border-radius: 50%; background: var(--text-faint); }
  .minimal .dot.missing { background: var(--danger); }
  .minimal .dot.invalid { background: var(--warning); }
  .minimal .dot.stale { background: var(--accent); }
  /* An unscaffolded node is hollow rather than coloured: it is not a problem to
     be fixed, it is a node nobody has written anything into yet. */
  .minimal .dot.empty { border: 1px solid var(--text-faint); background: none; }
  .minimal .name { overflow: hidden; color: var(--text-faint); font: 8px/1.3 ui-monospace, monospace; text-overflow: ellipsis; white-space: nowrap; }
  .hint { margin: 0; color: var(--text-faint); font-size: 8px; }
  .diagnostics { margin: 0; padding: 0; list-style: none; display: grid; gap: 2px; }
  .diagnostics li { display: grid; grid-template-columns: 44px 1fr; align-items: baseline; gap: 5px; }
  .diagnostics span { color: var(--text-faint); font: 8px/1.4 ui-monospace, monospace; }
  .diagnostics code { color: var(--warning); font: 9px/1.4 ui-monospace, monospace; white-space: pre-wrap; }
  .dropped { margin: 0; color: var(--warning); font-size: 8px; }
  .error { margin: 0; font-size: 8px; }
  .actions { display: flex; gap: 4px; }
  /* The icon buttons are square and do not stretch; the Output toggle, when there
     is one, takes whatever is left, because a count is the only thing in this row
     whose width depends on what it says. */
  .actions button.icon { flex: 0 0 24px; display: grid; place-items: center; padding: 0; }
  .actions button.toggle { flex: 1 1 0; }
  .actions button { height: 21px; border: 1px solid var(--border); border-radius: 3px; background: var(--surface); color: var(--text); font: 600 9px/1 'Segoe UI', sans-serif; cursor: pointer; }
  .actions button:hover:not(:disabled) { border-color: var(--accent); }
  .actions button:disabled { color: var(--text-faint); cursor: default; }
  .log { max-height: 120px; margin: 0; padding: 4px 0; overflow: auto; border: 1px solid var(--border); background: var(--canvas); list-style: none; }
  .log li { padding: 1px 7px; }
  .log code { color: var(--text); font: 9px/1.4 ui-monospace, monospace; white-space: pre-wrap; }
</style>
