<script lang="ts">
  /**
   * The Script Inspector: one script node's file, editable, on the canvas.
   *
   * ⚠️ IT IS THE SECOND EDITOR, NOT THE FIRST, AND IT IS BUILT LIKE ONE. VSCode
   * or Sublime remains where a script is written; this is for the fix you can see
   * from the graph - a sign flipped, a port renamed, a typo in a header - where
   * changing windows costs more than the edit does. Everything here follows from
   * that: one file, no project tree, no package management, and a save that
   * REFUSES rather than wins when the file has moved on underneath it.
   *
   * ⚠️ THE SOURCE IS FETCHED, NOT POLLED. The status verb runs every second and a
   * half for every script node on screen and deliberately carries no text; this
   * panel asks for the source when it opens a file and again only when the stamp
   * it is watching actually moves. A poll that carried the source would put a
   * whole file across the bridge, forever, so that a panel nobody has opened
   * could be up to date.
   */
  import {
    EMPTY_BUFFER,
    applyConflict,
    applySaved,
    diffAgainstIncoming,
    editBuffer,
    editorLanguageOf,
    isDirty,
    keepLocal,
    openBuffer,
    phaseOf,
    reconcile,
    saveBlockedReason,
    takeDisk,
    type ScriptBuffer,
  } from './buffer'
  import { tick } from 'svelte'
  import CodeEditor from './CodeEditor.svelte'
  import { EMPTY_SCRIPT_STATUS, SCRIPT_POLL_INTERVAL_MS, fileNameOf, type ScriptStatus } from './script'
  import {
    fetchScriptStatus,
    openScriptInEditor,
    readScriptSource,
    reloadScript,
    revealScriptInExplorer,
    writeScriptSource,
  } from './scriptBridge'

  let {
    nodeId,
    graphId,
    title,
    onclose,
    onreloaded,
  }: {
    nodeId: string
    graphId?: string
    /** The node's own label, so the panel says which node it is editing. */
    title: string
    onclose: () => void
    /** A reload landed and may have reshaped the node's ports. */
    onreloaded: (status: ScriptStatus) => void
  } = $props()

  let buffer = $state<ScriptBuffer>(EMPTY_BUFFER)
  let status = $state<ScriptStatus>(EMPTY_SCRIPT_STATUS)
  let busy = $state(false)
  let notice = $state('')
  let error = $state('')
  let showDiff = $state(false)
  let revealLine = $state(0)
  let logOpen = $state(false)

  /*
   * The stamp the buffer was last synced against. Compared with what status
   * reports so a save made in VSCode triggers exactly one source fetch - and the
   * size is part of it for the same reason it is part of ScriptStamp natively: a
   * fast edit-save-edit cycle can produce two files with the same mtime.
   */
  let syncedStamp = $state('')
  let loadedNode = $state('')

  const phase = $derived(phaseOf(buffer))
  const dirty = $derived(isDirty(buffer))
  const diff = $derived(diffAgainstIncoming(buffer))
  const blocked = $derived(saveBlockedReason(buffer))
  const fileName = $derived(buffer.path === '' ? 'No file' : fileNameOf(buffer.path))
  const language = $derived(editorLanguageOf(buffer))

  function stampOf(state: { modifiedAtMs: number; sizeBytes: number }): string {
    return `${state.modifiedAtMs}:${state.sizeBytes}`
  }

  async function guard(work: () => Promise<void>): Promise<void> {
    busy = true
    error = ''
    try {
      await work()
    } catch (failure) {
      error = failure instanceof Error ? failure.message : String(failure)
    } finally {
      busy = false
    }
  }

  async function load(): Promise<void> {
    await guard(async () => {
      const read = await readScriptSource(nodeId, graphId)
      buffer = openBuffer(read)
      syncedStamp = stampOf(read)
      showDiff = false
      notice = ''
    })
  }

  /*
   * The reconciliation, and the one place an external save reaches the buffer.
   * A CLEAN buffer takes the file silently - prompting on every VSCode save would
   * make the panel unusable next to the editor it is meant to complement. A DIRTY
   * one raises a conflict and touches nothing. buffer.ts decides which.
   */
  async function pullExternal(): Promise<void> {
    await guard(async () => {
      const read = await readScriptSource(nodeId, graphId)
      const before = buffer
      buffer = reconcile(before, read)
      syncedStamp = stampOf(read)
      if (buffer.incoming !== null) notice = ''
      else if (isDirty(before) === false && before.baseHash !== read.sourceHash) notice = 'Reloaded from disk.'
    })
  }

  async function save(): Promise<void> {
    if (blocked !== '') return
    await guard(async () => {
      const sent = buffer.text
      const result = await writeScriptSource(nodeId, sent, buffer.baseHash, graphId)
      if (result.conflict) {
        buffer = applyConflict(buffer, result)
        notice = ''
        return
      }
      buffer = applySaved(buffer, result, sent)
      syncedStamp = stampOf(result)
      // ⚠️ THE RELOAD IS PART OF SAVING, NOT A SEPARATE COURTESY. A script's
      // header declares the node's ports; a save that changed one leaves the
      // canvas showing an interface the file no longer has until something
      // reloads it. Reload is also the only verb that reports the edges it had
      // to drop, which is the one consequence of a save the user cannot see from
      // the text they just typed.
      const reloaded = await reloadScript(nodeId, graphId)
      status = reloaded
      onreloaded(reloaded)
      notice = reloaded.droppedEdges.length > 0 ? '' : 'Saved.'
    })
  }

  /*
   * The editor reveals a line when this value CHANGES, so it is cleared first -
   * without that, clicking the same problem a second time (after scrolling away
   * to look at something else) does nothing at all, which reads as a broken
   * button rather than as a subtlety of change detection.
   */
  async function goToLine(line: number): Promise<void> {
    revealLine = 0
    await tick()
    revealLine = line
  }

  function resolve(choice: 'mine' | 'disk'): void {
    buffer = choice === 'mine' ? keepLocal(buffer) : takeDisk(buffer)
    showDiff = false
    notice = choice === 'mine' ? 'Keeping this version. Save to write it over the file.' : 'Took the version on disk.'
  }

  async function reloadNode(): Promise<void> {
    await guard(async () => {
      const reloaded = await reloadScript(nodeId, graphId)
      status = reloaded
      onreloaded(reloaded)
    })
  }

  async function shell(work: () => Promise<void>): Promise<void> {
    await guard(work)
  }

  /* Switching nodes opens that node's file. Guarded on the id rather than run on
     every render, so a poll landing does not re-read the file under the cursor. */
  $effect(() => {
    if (nodeId === loadedNode) return
    loadedNode = nodeId
    buffer = EMPTY_BUFFER
    status = EMPTY_SCRIPT_STATUS
    notice = ''
    void load()
  })

  $effect(() => {
    let cancelled = false
    async function poll(): Promise<void> {
      try {
        const next = await fetchScriptStatus(nodeId, graphId)
        if (cancelled) return
        status = next
        // The source is fetched only when the file it belongs to actually moved.
        if (next.path !== '' && stampOf(next) !== syncedStamp && !busy) await pullExternal()
      } catch {
        /* Swallowed: this runs on a timer, and a bridge that is briefly away
           should not paint an error over a panel that is otherwise fine. Every
           action the user pressed reports its own failure. */
      }
    }
    void poll()
    const timer = window.setInterval(() => {
      if (!document.hidden) void poll()
    }, SCRIPT_POLL_INTERVAL_MS)
    return () => {
      cancelled = true
      window.clearInterval(timer)
    }
  })

  /*
   * ⚠️ THE ONE THING THAT MUST NOT HAPPEN SILENTLY IS CLOSING OVER UNSAVED WORK.
   * The panel is a floating thing on a canvas, and Escape or a stray click on
   * Close is easy; a buffer discarded that way is gone with no undo, because the
   * editor's history dies with the component.
   */
  function requestClose(): void {
    if (!dirty) {
      onclose()
      return
    }
    notice = 'Unsaved changes. Save, or press Discard to close anyway.'
  }
</script>

<aside class="script-editor nodrag nowheel" aria-label="Script editor">
  <header>
    <div class="identity">
      <span>{title}</span>
      <strong title={buffer.path}>{fileName}{dirty ? ' •' : ''}</strong>
    </div>
    <button class="close" onclick={requestClose} aria-label="Close the script editor">Close</button>
  </header>

  {#if buffer.path === ''}
    <p class="empty">This node has no file yet. Type a path on the node and press Create.</p>
  {:else}
    {#if buffer.incoming !== null}
      <!--
        The conflict. Both versions exist and NEITHER is thrown away by anything
        but a press - which is the entire reason the palette is allowed to write
        a file an external editor also has open.
      -->
      <section class="conflict">
        <p><strong>{fileName} changed on disk</strong> while you were editing it here.</p>
        <div class="choices">
          <button onclick={() => resolve('mine')}>Keep mine</button>
          <button onclick={() => resolve('disk')}>Use the file</button>
          <button class="ghost" onclick={() => (showDiff = !showDiff)}>{showDiff ? 'Hide' : 'Compare'}</button>
        </div>
        {#if showDiff && diff !== null}
          <div class="diff">
            <p>First difference at line {diff.firstChangedLine}.</p>
            <div>
              <h3>Here</h3>
              <pre>{diff.mine.join('\n') || '(nothing)'}</pre>
            </div>
            <div>
              <h3>On disk</h3>
              <pre>{diff.theirs.join('\n') || '(nothing)'}</pre>
            </div>
          </div>
        {/if}
      </section>
    {/if}

    <div class="editor">
      <CodeEditor
        text={buffer.text}
        {language}
        diagnostics={status.diagnostics.map((item) => ({ line: item.line, message: item.message }))}
        readOnly={buffer.incoming !== null}
        {revealLine}
        onchange={(text) => { buffer = editBuffer(buffer, text); notice = '' }}
        onsave={() => void save()}
      />
    </div>

    {#if status.diagnostics.length > 0}
      <!--
        Header problems, from the node's own parse. Clicking one goes to its line,
        which is the difference between a diagnostic and a complaint.
      -->
      <ul class="problems">
        {#each status.diagnostics as diagnostic}
          <li>
            <button onclick={() => void goToLine(diagnostic.line)} disabled={diagnostic.line < 1}>
              <span>{diagnostic.line > 0 ? `line ${diagnostic.line}` : 'file'}</span><code>{diagnostic.message}</code>
            </button>
          </li>
        {/each}
      </ul>
    {/if}

    {#if status.droppedEdges.length > 0}
      <p class="dropped">
        {status.droppedEdges.length} connection{status.droppedEdges.length === 1 ? '' : 's'} dropped: the port
        {status.droppedEdges.length === 1 ? 'it used' : 'they used'} no longer exists or changed type.
      </p>
    {/if}

    {#if error !== ''}<p class="error">{error}</p>{/if}
    {#if error === '' && notice !== ''}<p class="notice">{notice}</p>{/if}

    <footer>
      <button class="primary" disabled={busy || blocked !== ''} title={blocked} onclick={() => void save()}>
        {busy ? 'Working…' : 'Save'}
      </button>
      <button disabled={busy} onclick={() => void pullExternal()} title="Read the file again">Revert</button>
      <button disabled={busy} onclick={() => void reloadNode()} title="Re-read the header and reshape the node's ports">
        Reload node
      </button>
      <button disabled={busy || !buffer.exists} onclick={() => void shell(() => openScriptInEditor(nodeId, graphId))}>
        Open externally
      </button>
      <button disabled={busy || !buffer.exists} onclick={() => void shell(() => revealScriptInExplorer(nodeId, graphId))}>
        Show in Explorer
      </button>
      {#if status.log.length > 0}
        <button class="ghost" onclick={() => (logOpen = !logOpen)}>Output ({status.log.length})</button>
      {/if}
      {#if dirty}
        <button class="ghost danger" onclick={onclose} title="Close and lose the unsaved changes">Discard</button>
      {/if}
      <span class="phase" class:conflict={phase === 'conflict'} class:dirty={phase === 'dirty'}>
        {#if phase === 'conflict'}Conflict{:else if phase === 'dirty'}Unsaved{:else}Saved{/if}
      </span>
    </footer>

    {#if logOpen && status.log.length > 0}
      <ol class="log">{#each status.log as line}<li><code>{line}</code></li>{/each}</ol>
    {/if}
  {/if}
</aside>

<style>
  .script-editor { position: absolute; z-index: 9; top: 12px; right: 12px; display: flex; width: min(560px, calc(100% - 24px)); max-height: calc(100% - 24px); flex-direction: column; overflow: hidden; border: 1px solid var(--border); border-radius: 5px; background: var(--surface); box-shadow: 0 18px 60px rgb(0 0 0 / 45%); color: var(--text); font: 11px/1.45 'Segoe UI', sans-serif; }
  header { display: flex; align-items: center; justify-content: space-between; padding: 9px 11px; border-bottom: 1px solid var(--border); }
  .identity { display: grid; min-width: 0; }
  .identity span { color: var(--text-faint); font-size: 9px; letter-spacing: .08em; text-transform: uppercase; }
  .identity strong { overflow: hidden; font-size: 13px; text-overflow: ellipsis; white-space: nowrap; }
  .empty { margin: 0; padding: 18px 14px; color: var(--text-faint); }
  /* The editor is the only thing that grows, so a long script scrolls inside it
     rather than pushing the toolbar off the bottom of the panel. */
  .editor { min-height: 220px; flex: 1 1 auto; overflow: hidden; border-bottom: 1px solid var(--border); }
  .conflict { padding: 9px 11px; border-bottom: 1px solid var(--border); background: color-mix(in srgb, #d98f3f 14%, transparent); }
  .conflict p { margin: 0 0 7px; }
  .choices { display: flex; gap: 6px; }
  .diff { display: grid; gap: 6px; margin-top: 9px; }
  .diff p { margin: 0; color: var(--text-faint); font-size: 10px; }
  .diff h3 { margin: 0 0 3px; color: var(--text-faint); font-size: 9px; letter-spacing: .08em; text-transform: uppercase; }
  .diff pre { max-height: 110px; margin: 0; padding: 6px 8px; overflow: auto; border: 1px solid var(--border); background: var(--canvas); font: 10px/1.45 ui-monospace, monospace; }
  .problems { max-height: 96px; margin: 0; padding: 6px 0; overflow: auto; border-bottom: 1px solid var(--border); list-style: none; }
  .problems button { display: grid; width: 100%; padding: 2px 11px; border: 0; background: none; color: inherit; cursor: pointer; grid-template-columns: 52px 1fr; text-align: left; }
  .problems button:hover:not(:disabled) { background: color-mix(in srgb, var(--text) 7%, transparent); }
  .problems span { color: var(--text-faint); font: 9px ui-monospace, monospace; }
  .problems code { color: #d98f3f; font: 10px/1.4 ui-monospace, monospace; white-space: pre-wrap; }
  .dropped { margin: 0; padding: 7px 11px; border-bottom: 1px solid var(--border); color: #d98f3f; font-size: 10px; }
  .error, .notice { margin: 0; padding: 7px 11px; border-bottom: 1px solid var(--border); font-size: 10px; }
  .error { color: #d05f5f; }
  .notice { color: var(--text-faint); }
  footer { display: flex; align-items: center; padding: 8px 11px; gap: 6px; flex-wrap: wrap; }
  button { height: 23px; padding: 0 9px; border: 1px solid var(--border); border-radius: 3px; background: var(--canvas); color: var(--text); font: 600 10px/1 'Segoe UI', sans-serif; cursor: pointer; }
  button:hover:not(:disabled) { border-color: var(--accent, #4c8cd8); }
  button:disabled { color: var(--text-faint); cursor: default; }
  button.primary { border-color: var(--accent, #4c8cd8); background: var(--accent, #4c8cd8); color: #fff; }
  button.primary:disabled { border-color: var(--border); background: var(--canvas); color: var(--text-faint); }
  button.ghost { border-color: transparent; }
  button.danger { color: #d05f5f; }
  .phase { margin-left: auto; color: var(--text-faint); font-size: 9px; letter-spacing: .06em; text-transform: uppercase; }
  .phase.dirty { color: var(--accent, #4c8cd8); }
  .phase.conflict { color: #d98f3f; }
  .log { max-height: 120px; margin: 0; padding: 5px 0; overflow: auto; border-top: 1px solid var(--border); background: var(--canvas); list-style: none; }
  .log li { padding: 1px 11px; }
  .log code { font: 10px/1.45 ui-monospace, monospace; white-space: pre-wrap; }
  @media (max-width: 620px) { .script-editor { top: 6px; right: 6px; width: calc(100% - 12px); max-height: calc(100% - 12px); } }
</style>
