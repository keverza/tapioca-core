<script lang="ts">
  /**
   * The Script Inspector: one script node's FOLDER, editable, on the canvas.
   *
   * ⚠️ IT IS THE SECOND EDITOR, NOT THE FIRST, AND IT IS BUILT LIKE ONE. VSCode
   * or Sublime remains where a script is written; this is for the fix you can see
   * from the graph - a sign flipped, a port renamed, a typo in a header - where
   * changing windows costs more than the edit does. Everything here follows from
   * that: one node folder, no project tree, no package management, and a save
   * that REFUSES rather than wins when the file has moved on underneath it.
   *
   * ⚠️ ONE BUFFER PER FILE, AND THE RULES ARE PER BUFFER. A conflict in
   * `calculations.py` has nothing to do with `main.py`; merging them would either
   * block a save on an unrelated file or resolve two conflicts with one press.
   * buffer.ts holds the rules and ScriptTabs holds the map; this component draws
   * what they say.
   *
   * ⚠️ AND SOURCE IS FETCHED, NOT POLLED. The status verb runs every second and a
   * half for every script node on screen and deliberately carries no text; this
   * panel asks for a file when a tab is opened and again only when the folder's
   * stamp actually moves. A poll that carried source would put whole files across
   * the bridge, forever, so that a panel nobody has opened could be up to date.
   */
  import { tick } from 'svelte'
  import {
    EMPTY_BUFFER,
    EMPTY_TABS,
    applyConflict,
    applySaved,
    bufferFor,
    diffAgainstIncoming,
    dirtyFiles,
    editBuffer,
    editorLanguageOf,
    isDirty,
    keepLocal,
    newFileNameError,
    openBuffer,
    phaseOf,
    reconcile,
    saveBlockedReason,
    takeDisk,
    withBuffer,
    type ScriptTabs,
  } from './buffer'
  import CodeEditor from './CodeEditor.svelte'
  import { EMPTY_SCRIPT_STATUS, SCRIPT_POLL_INTERVAL_MS, fileNameOf, workspaceNameOf, type ScriptStatus } from './script'
  import {
    addScriptFile,
    createScript,
    fetchScriptLibrary,
    fetchScriptStatus,
    openScriptInEditor,
    openScriptLibraryFolder,
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

  let tabs = $state<ScriptTabs>(EMPTY_TABS)
  let status = $state<ScriptStatus>(EMPTY_SCRIPT_STATUS)
  let busy = $state(false)
  let notice = $state('')
  let error = $state('')
  let showDiff = $state(false)
  let revealLine = $state(0)
  let logOpen = $state(false)
  let rootsOpen = $state(false)
  let adding = $state(false)
  let newName = $state('')

  /*
   * The folder stamp the open buffers were last reconciled against. Compared with
   * what status reports, so a save made in VSCode triggers exactly one fetch per
   * open tab - and the size is part of it for the same reason it is part of the
   * native stamp: a fast edit-save-edit cycle can produce two files with the same
   * mtime.
   */
  let syncedStamp = $state('')
  let loadedNode = $state('')

  /**
   * The starter script for a node that has no folder yet, before it has one.
   *
   * ⚠️ A NEW SCRIPT NODE WRITES NOTHING TO DISK UNTIL THIS IS SAVED, AND THAT IS
   * THE WHOLE POINT OF THE DRAFT. Scaffolding on placement would mean a canvas
   * somebody spent an hour experimenting on left twenty abandoned folders in
   * their library; refusing to open an editor until they had created one meant
   * the first thing a new node asked for was a decision about a filename. So the
   * editor opens on the template immediately, the user plays with it, and the
   * folder comes into existence at the moment there is something worth keeping.
   *
   * The text comes from the NATIVE side, which is also what Create writes - see
   * ScriptTemplateSource. A copy of the template in this bundle would be a
   * starter script that quietly stopped matching the one on disk.
   */
  let draft = $state<{ name: string; text: string; language: 'python' | 'javascript' } | null>(null)

  /** Whether the starter script has been asked for on THIS node. See the effect. */
  let draftAsked = $state(false)

  /**
   * How wide the panel is, in pixels, remembered per browser.
   *
   * ⚠️ WIDTH ONLY. The panel takes the canvas's full HEIGHT and always has to:
   * it is a code editor, the thing it is short of is lines, and a height someone
   * has to drag out every time they open it is a height that is wrong on every
   * node they open next. Width is the axis where the trade is real - a wide panel
   * reads long lines, a narrow one leaves the graph visible beside it - so that
   * is the one the user gets to make, and it is remembered because they will make
   * the same choice every time otherwise.
   */
  const MIN_WIDTH = 320
  const DEFAULT_WIDTH = 560
  const WIDTH_KEY = 'tapioca.scriptEditor.width'
  let width = $state(DEFAULT_WIDTH)
  let dragging = $state(false)

  $effect(() => {
    // Read once, and tolerantly: a stored width from a much larger window must
    // not open a panel wider than the canvas it is in.
    const stored = Number(window.localStorage?.getItem(WIDTH_KEY) ?? '')
    if (Number.isFinite(stored) && stored >= MIN_WIDTH) width = stored
  })

  function startResize(event: PointerEvent): void {
    event.preventDefault()
    dragging = true
    const startX = event.clientX
    const startWidth = width
    const target = event.currentTarget as HTMLElement
    target.setPointerCapture(event.pointerId)

    const move = (moved: PointerEvent): void => {
      // The handle is on the LEFT edge of a right-anchored panel, so dragging
      // left (a negative delta) makes it wider.
      const next = startWidth + (startX - moved.clientX)
      width = Math.max(MIN_WIDTH, Math.min(next, window.innerWidth - 40))
    }
    const stop = (): void => {
      dragging = false
      target.releasePointerCapture?.(event.pointerId)
      target.removeEventListener('pointermove', move)
      target.removeEventListener('pointerup', stop)
      target.removeEventListener('pointercancel', stop)
      try {
        window.localStorage?.setItem(WIDTH_KEY, String(Math.round(width)))
      } catch {
        /* A palette with storage denied resizes fine and forgets. Not worth a
           message: the user is looking at the size they just chose. */
      }
    }
    target.addEventListener('pointermove', move)
    target.addEventListener('pointerup', stop)
    target.addEventListener('pointercancel', stop)
  }

  const active = $derived(tabs.active)
  const buffer = $derived(bufferFor(tabs, active))
  const phase = $derived(phaseOf(buffer))
  const diff = $derived(diffAgainstIncoming(buffer))
  const blocked = $derived(saveBlockedReason(buffer))
  const language = $derived(editorLanguageOf(buffer))
  const unsaved = $derived(dirtyFiles(tabs))
  const nodeName = $derived(workspaceNameOf(status))
  const entryName = $derived(status.entryFile === '' ? 'main.py' : fileNameOf(status.entryFile))
  /** The tab strip. `files` comes from the same verb as everything else, so a
   *  node's tabs cannot disagree with the node they belong to. */
  const files = $derived(status.files)
  const localNames = $derived(files.filter((file) => !file.shared).map((file) => file.name))
  const nameError = $derived(adding ? newFileNameError(newName, language, localNames) : '')
  /** The active tab's own label. `''` is the entry file, which is what the panel
   *  opens with - it does not know main.py from main.js until status arrives. */
  const activeLabel = $derived(active === '' ? entryName : active)
  const activeShared = $derived(files.find((file) => file.name === active)?.shared ?? false)

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

  /** Opens one file as a tab, reading it if this is the first time. */
  async function open(file: string): Promise<void> {
    tabs = { ...tabs, active: file }
    showDiff = false
    if (tabs.open[file] !== undefined) return
    await guard(async () => {
      const read = await readScriptSource(nodeId, file, graphId)
      // ⚠️ THE RESPONSE NAMES THE FILE IT IS FOR. A read that lands after the
      // user switched tabs must not be painted into whatever is open now.
      if (read.file !== file) return
      tabs = withBuffer(tabs, file, openBuffer(read))
      notice = ''
    })
  }

  /*
   * The reconciliation, and the one place an external save reaches a buffer. A
   * CLEAN buffer takes the file silently - prompting on every VSCode save would
   * make the panel unusable beside the editor it complements. A DIRTY one raises
   * a conflict and touches nothing. buffer.ts decides which, per file.
   */
  async function pullExternal(): Promise<void> {
    await guard(async () => {
      let reloaded = 0
      for (const file of Object.keys(tabs.open)) {
        const read = await readScriptSource(nodeId, file, graphId)
        if (read.file !== file) continue
        const before = bufferFor(tabs, file)
        const after = reconcile(before, read)
        if (after !== before) reloaded += 1
        tabs = withBuffer(tabs, file, after)
      }
      syncedStamp = stampOf(status)
      notice = reloaded > 0 && !dirtyFiles(tabs).length ? 'Reloaded from disk.' : ''
    })
  }

  async function save(): Promise<void> {
    if (blocked !== '') return
    const file = active
    await guard(async () => {
      const sent = buffer.text
      const result = await writeScriptSource(nodeId, file, sent, buffer.baseHash, graphId)
      if (result.conflict) {
        tabs = withBuffer(tabs, file, applyConflict(bufferFor(tabs, file), result))
        notice = ''
        return
      }
      tabs = withBuffer(tabs, file, applySaved(bufferFor(tabs, file), result, sent))
      // ⚠️ THE RELOAD IS PART OF SAVING, NOT A COURTESY. main.py's header declares
      // the node's ports, and a HELPER can change what the node computes just as
      // completely - so both reload. Reload is also the only verb that reports the
      // edges it had to drop, which is the one consequence of a save the user
      // cannot see in the text they just typed.
      const reloaded = await reloadScript(nodeId, graphId)
      status = reloaded
      syncedStamp = stampOf(reloaded)
      onreloaded(reloaded)
      notice = reloaded.droppedEdges.length > 0 ? '' : 'Saved.'
    })
  }

  /*
   * The editor reveals a line when this value CHANGES, so it is cleared first -
   * without that, clicking the same problem a second time (after scrolling away
   * to look at something else) does nothing, which reads as a broken button.
   *
   * Diagnostics are the ENTRY FILE's: the header parser reports lines in main.py.
   * So this switches to that tab before scrolling, rather than highlighting a
   * line number in whatever file happened to be open.
   */
  async function goToLine(line: number): Promise<void> {
    if (active !== '') await open('')
    revealLine = 0
    await tick()
    revealLine = line
  }

  function resolve(choice: 'mine' | 'disk'): void {
    const file = active
    tabs = withBuffer(tabs, file, choice === 'mine' ? keepLocal(buffer) : takeDisk(buffer))
    showDiff = false
    notice = choice === 'mine' ? 'Keeping this version. Save to write it over the file.' : 'Took the version on disk.'
  }

  async function createFile(): Promise<void> {
    if (nameError !== '') return
    const name = newName.trim()
    await guard(async () => {
      await addScriptFile(nodeId, name, graphId)
      adding = false
      newName = ''
      // Refetch so the new file joins the tab strip through the SAME listing
      // every other tab came from, rather than being appended locally and
      // disagreeing with the node the moment anything else changes.
      status = await fetchScriptStatus(nodeId, graphId)
      syncedStamp = stampOf(status)
      await open(name)
    })
  }

  async function reloadNode(): Promise<void> {
    await guard(async () => {
      const reloaded = await reloadScript(nodeId, graphId)
      status = reloaded
      syncedStamp = stampOf(reloaded)
      onreloaded(reloaded)
    })
  }

  /* Switching nodes opens that node's folder. Guarded on the id rather than run
     on every render, so a poll landing does not re-read under the cursor. */
  $effect(() => {
    if (nodeId === loadedNode) return
    loadedNode = nodeId
    tabs = EMPTY_TABS
    status = EMPTY_SCRIPT_STATUS
    syncedStamp = ''
    notice = ''
    adding = false
    draft = null
    draftAsked = false
    void open('')
  })

  /**
   * A node with no folder gets the starter script to play with.
   *
   * ⚠️ GUARDED ON HAVING ASKED, NOT ON HAVING AN ANSWER. `guard` toggles `busy`,
   * which this effect reads, so a fetch that FAILED would leave `draft` null,
   * flip `busy` back, re-run this effect and ask again - forever, at whatever
   * rate the bridge is failing at. The flag is cleared when the node changes,
   * which is the only thing that makes the answer worth asking for again.
   */
  $effect(() => {
    if (status.path !== '' || draftAsked || busy) return
    draftAsked = true
    void guard(async () => {
      const library = await fetchScriptLibrary(nodeId, graphId)
      if (status.path !== '') return // A create landed while this was in flight.
      draft = {
        name: library.suggestedName,
        text: library.template,
        // From the node TYPE, natively: a .js node's starter script is not Python
        // and must not be highlighted as though it were.
        language: library.language === 'javascript' ? 'javascript' : 'python',
      }
    })
  })

  /**
   * Save the draft, which is the moment the folder comes into existence.
   *
   * Create writes the template, then the user's edits go over it through the
   * ordinary guarded write - rather than a create-with-contents verb, which would
   * be a second way for a file to be brought into being and a second place for
   * "does this overwrite?" to be answered.
   */
  async function saveDraft(): Promise<void> {
    const pending = draft
    if (pending === null) return
    const name = pending.name.trim()
    if (name === '') {
      error = 'Name this script first'
      return
    }
    await guard(async () => {
      const created = await createScript(nodeId, name, graphId)
      const read = await readScriptSource(nodeId, '', graphId)
      if (read.ok && read.source !== pending.text) {
        const written = await writeScriptSource(nodeId, '', pending.text, read.sourceHash, graphId)
        if (written.conflict) {
          // Effectively impossible - the file was created a moment ago - but the
          // guard is the guard, and silently discarding the user's text because
          // the impossible happened is exactly the failure it exists to prevent.
          error = 'The new file changed while it was being written. Nothing was lost; try saving again.'
          return
        }
      }
      draft = null
      status = created
      syncedStamp = stampOf(created)
      onreloaded(created)
      await open('')
      const reloaded = await reloadScript(nodeId, graphId)
      status = reloaded
      onreloaded(reloaded)
      notice = `Saved as ${name}.`
    })
  }

  $effect(() => {
    let cancelled = false
    async function poll(): Promise<void> {
      try {
        const next = await fetchScriptStatus(nodeId, graphId)
        if (cancelled) return
        status = next
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
   * ⚠️ THE ONE THING THAT MUST NOT HAPPEN SILENTLY IS CLOSING OVER UNSAVED WORK,
   * and with tabs it must name the files. The buffers die with this component and
   * there is no undo across a close; a user who edited three files and is looking
   * at the fourth would otherwise lose two they had stopped thinking about.
   */
  function requestClose(): void {
    if (unsaved.length === 0) {
      onclose()
      return
    }
    notice = `Unsaved: ${unsaved.join(', ')}. Save, or press Discard to close anyway.`
  }
</script>

<aside class="script-editor nodrag nowheel" aria-label="Script editor" style={`width: ${width}px`}>
  <!--
    The one axis the user gets to choose. Height is not offered: this is a code
    editor and it always wants every line the canvas can give it.
  -->
  <div
    class="grip"
    class:dragging
    role="separator"
    aria-label="Resize the script editor"
    aria-orientation="vertical"
    onpointerdown={startResize}
  ></div>

  <div class="panel">
  <header>
    <div class="identity">
      <span>{title}</span>
      <strong title={status.workspaceRoot}>{nodeName === '' ? (draft?.name ?? 'New script') : nodeName}{unsaved.length > 0 ? ' •' : ''}</strong>
    </div>
    <button class="close" onclick={requestClose} aria-label="Close the script editor">Close</button>
  </header>

  {#if status.path === ''}
    <!--
      The draft. A brand-new node opens straight into a runnable starter script,
      and nothing is on disk until it is saved - see the note on `draft`. The
      folder name is a field here rather than a decision to make first, because
      the interesting thing to do with a new script node is read the template and
      change a line, not think of a filename.
    -->
    <label class="draft-name">
      <span>Save as</span>
      <input
        type="text"
        spellcheck="false"
        placeholder="new_script"
        value={draft?.name ?? ''}
        oninput={(event) => { if (draft !== null) draft = { ...draft, name: event.currentTarget.value } }}
      />
    </label>
    <div class="editor">
      <CodeEditor
        text={draft?.text ?? ''}
        language={draft?.language ?? "python"}
        readOnly={draft === null}
        onchange={(text) => { if (draft !== null) draft = { ...draft, text }; error = '' }}
        onsave={() => void saveDraft()}
      />
    </div>
    {#if error !== ''}<p class="error">{error}</p>{/if}
    {#if error === '' && notice !== ''}<p class="notice">{notice}</p>{/if}
    <footer>
      <button class="primary" disabled={busy || draft === null} onclick={() => void saveDraft()}>
        {busy ? 'Working…' : 'Create and save'}
      </button>
      <button disabled={busy} onclick={() => void guard(() => openScriptLibraryFolder())} title="Where every script node folder lives">
        Workflows folder
      </button>
      <span class="phase">Not saved yet</span>
    </footer>
  {:else}
    <!--
      The tab strip. A shared file is marked and never mixed in with the node's
      own: an edit to libs/geometry.py reaches every node that imports it, which
      is exactly the change someone makes without meaning to.
    -->
    <nav class="tabs" aria-label="Files in this node">
      {#each files as file (file.name)}
        <button
          class="tab"
          class:active={(file.entry && active === '') || active === file.name}
          class:shared={file.shared}
          class:dirty={isDirty(bufferFor(tabs, file.entry ? '' : file.name))}
          title={file.shared ? `${file.name} — shared by every node` : file.name}
          onclick={() => void open(file.entry ? '' : file.name)}
        >{file.shared ? file.name.replace('libs/', '') : file.name}{file.shared ? ' ↗' : ''}</button>
      {/each}
      {#if adding}
        <span class="adding">
          <!-- svelte-ignore a11y_autofocus -->
          <input
            autofocus
            spellcheck="false"
            placeholder={language === 'javascript' ? 'helpers.js' : 'helpers.py'}
            bind:value={newName}
            onkeydown={(event) => {
              if (event.key === 'Enter') void createFile()
              if (event.key === 'Escape') { adding = false; newName = '' }
            }}
          />
          <button disabled={busy || nameError !== ''} onclick={() => void createFile()}>Add</button>
        </span>
      {:else}
        <button class="tab add" title="Add a file to this node's folder" onclick={() => { adding = true; newName = '' }}>+</button>
      {/if}
    </nav>
    {#if adding && nameError !== ''}<p class="hint">{nameError}</p>{/if}

    {#if activeShared}
      <p class="hint shared-note">
        Shared by every node that imports it. Saving changes them all.
      </p>
    {/if}

    {#if buffer.incoming !== null}
      <!--
        The conflict. Both versions exist and NEITHER is thrown away by anything
        but a press - which is the entire reason the palette is allowed to write
        a file an external editor also has open.
      -->
      <section class="conflict">
        <p><strong>{activeLabel} changed on disk</strong> while you were editing it here.</p>
        <div class="choices">
          <button onclick={() => resolve('mine')}>Keep mine</button>
          <button onclick={() => resolve('disk')}>Use the file</button>
          <button class="ghost" onclick={() => (showDiff = !showDiff)}>{showDiff ? 'Hide' : 'Compare'}</button>
        </div>
        {#if showDiff && diff !== null}
          <div class="diff">
            <p>First difference at line {diff.firstChangedLine}.</p>
            <div><h3>Here</h3><pre>{diff.mine.join('\n') || '(nothing)'}</pre></div>
            <div><h3>On disk</h3><pre>{diff.theirs.join('\n') || '(nothing)'}</pre></div>
          </div>
        {/if}
      </section>
    {/if}

    <div class="editor">
      <CodeEditor
        text={buffer.text}
        {language}
        diagnostics={active === '' ? status.diagnostics.map((item) => ({ line: item.line, message: item.message })) : []}
        readOnly={buffer.incoming !== null}
        {revealLine}
        onchange={(text) => { tabs = withBuffer(tabs, active, editBuffer(buffer, text)); notice = '' }}
        onsave={() => void save()}
      />
    </div>

    {#if status.diagnostics.length > 0}
      <!--
        Header problems, from the node's own parse of the ENTRY file. Clicking one
        opens that tab and goes to its line, which is the difference between a
        diagnostic and a complaint.
      -->
      <ul class="problems">
        {#each status.diagnostics as diagnostic}
          <li>
            <button onclick={() => void goToLine(diagnostic.line)} disabled={diagnostic.line < 1}>
              <span>{diagnostic.line > 0 ? `${entryName}:${diagnostic.line}` : entryName}</span><code>{diagnostic.message}</code>
            </button>
          </li>
        {/each}
      </ul>
    {/if}

    {#if status.migratedFrom !== ''}
      <!--
        Said out loud because it MOVED the user's file. The graph looks unchanged
        and the filesystem does not, and the editor they had it open in still
        points at where it used to be.
      -->
      <p class="migrated">Converted to a folder: {status.migratedFrom} is now {entryName} inside {nodeName}.</p>
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
      <button disabled={busy} onclick={() => void pullExternal()} title="Read every open file again">Revert</button>
      <button disabled={busy} onclick={() => void reloadNode()} title="Re-read the header and reshape the node's ports">
        Reload node
      </button>
      <button disabled={busy} onclick={() => void guard(() => openScriptInEditor(nodeId, graphId))} title="Open the node's folder in your editor">
        Open folder
      </button>
      <button disabled={busy} onclick={() => void guard(() => revealScriptInExplorer(nodeId, graphId))}>Explorer</button>
      <!--
        The LIBRARY, not this node's folder, and both are worth a button.
        "Explorer" shows you where this script is; this one shows you where every
        script is - which is what you want when you are about to add one by hand,
        copy one in, or find out why the picker is empty. It lives here rather
        than on the node because the node's action row is four icons wide and this
        is a thing you do once a session, not once a minute.
      -->
      <button disabled={busy} onclick={() => void guard(() => openScriptLibraryFolder())} title="Where every script node folder lives">
        Workflows folder
      </button>
      {#if status.log.length > 0}
        <button class="ghost" onclick={() => (logOpen = !logOpen)}>Output ({status.log.length})</button>
      {/if}
      {#if status.importRoots.length > 0}
        <button class="ghost" onclick={() => (rootsOpen = !rootsOpen)} title="Where this node's imports resolve from">Imports</button>
      {/if}
      {#if unsaved.length > 0}
        <button class="ghost danger" onclick={onclose} title={`Close and lose: ${unsaved.join(', ')}`}>Discard</button>
      {/if}
      <span class="phase" class:conflict={phase === 'conflict'} class:dirty={phase === 'dirty'}>
        {#if phase === 'conflict'}Conflict{:else if phase === 'dirty'}Unsaved{:else}Saved{/if}
      </span>
    </footer>

    {#if rootsOpen}
      <!--
        ⚠️ SHOWN RATHER THAN EXPLAINED. "Why does `import calculations` not
        resolve" is the question this feature will generate most, and the answer
        is always in this list - the node's folder first, then the shared library.
        Tapioca's interpreter contributes the rest of sys.path, which is how numpy
        and the others import with no per-node environment.
      -->
      <ol class="roots">
        {#each status.importRoots as root}<li><code>{root}</code></li>{/each}
        <li class="note"><code>…and Tapioca's own runtime, for installed packages</code></li>
      </ol>
    {/if}

    {#if logOpen && status.log.length > 0}
      <ol class="log">{#each status.log as line}<li><code>{line}</code></li>{/each}</ol>
    {/if}
  {/if}
  </div>
</aside>

<style>
  /*
   * ⚠️ FULL CANVAS HEIGHT, ALWAYS, AND FLUSH TO THE EDGE. It is a code editor;
   * what it is short of is lines. The panel used to float with a margin and a
   * max-height, which spent three rows of a screen on shadow in order to look
   * like a card - and left the buffer with a viewport a third of the window on a
   * laptop. `inset` pins it top to bottom and the radius comes off the edges that
   * now touch the canvas.
   *
   * The width is inline, from the grip. `max-width` is the only clamp left here,
   * so a stored width from a much larger window cannot fill the whole palette.
   */
  .script-editor { position: absolute; z-index: 9; inset: 0 0 0 auto; display: flex; max-width: calc(100% - 40px); border-left: 1px solid var(--border); background: var(--surface); box-shadow: -12px 0 44px rgb(0 0 0 / 38%); color: var(--text); font: 11px/1.45 'Segoe UI', sans-serif; }
  /* The whole panel scrolls nothing; the editor inside it does. */
  .panel { display: flex; min-width: 0; flex: 1 1 auto; flex-direction: column; overflow: hidden; }
  /*
   * The resize handle: a hit target wider than the line it draws, because a 1px
   * grab area is a 1px grab area. It sits ON the border rather than beside it, so
   * the panel's left edge IS the thing you drag.
   */
  .grip { position: absolute; z-index: 1; top: 0; bottom: 0; left: -3px; width: 7px; cursor: ew-resize; touch-action: none; }
  .grip:hover::after, .grip.dragging::after { position: absolute; inset: 0 3px; background: var(--accent); content: ''; }
  .draft-name { display: grid; align-items: center; padding: 8px 11px; border-bottom: 1px solid var(--border); gap: 7px; grid-template-columns: 48px 1fr; }
  .draft-name span { color: var(--text-faint); font-size: 9px; letter-spacing: .06em; text-transform: uppercase; }
  .draft-name input { min-width: 0; height: 22px; padding: 0 6px; border: 1px solid var(--border); border-radius: 3px; background: var(--canvas); color: var(--text); font: 11px ui-monospace, monospace; }
  header { display: flex; align-items: center; justify-content: space-between; padding: 9px 11px; border-bottom: 1px solid var(--border); }
  .identity { display: grid; min-width: 0; }
  .identity span { color: var(--text-faint); font-size: 9px; letter-spacing: .08em; text-transform: uppercase; }
  .identity strong { overflow: hidden; font-size: 13px; text-overflow: ellipsis; white-space: nowrap; }
  /* The strip scrolls rather than wraps: a node with eight helpers must not push
     the editor down the panel, and tabs that reflowed as one was added would move
     under the pointer. */
  .tabs { display: flex; overflow-x: auto; align-items: center; padding: 5px 7px 0; border-bottom: 1px solid var(--border); gap: 3px; }
  .tab { flex: 0 0 auto; height: 22px; padding: 0 8px; border: 1px solid transparent; border-bottom: 0; border-radius: 3px 3px 0 0; background: none; color: var(--text-faint); font: 10px/1 ui-monospace, monospace; cursor: pointer; }
  .tab:hover { color: var(--text); }
  .tab.active { border-color: var(--border); background: var(--canvas); color: var(--text); }
  /* A dirty tab is marked, so a file edited three tabs ago is not invisible. */
  .tab.dirty::after { content: ' •'; color: var(--accent); }
  .tab.shared { font-style: italic; }
  .tab.add { color: var(--text-faint); font-size: 13px; }
  .adding { display: flex; flex: 0 0 auto; align-items: center; gap: 3px; }
  .adding input { width: 110px; height: 20px; padding: 0 5px; border: 1px solid var(--border); border-radius: 3px; background: var(--canvas); color: var(--text); font: 10px ui-monospace, monospace; }
  .adding button { height: 20px; padding: 0 7px; }
  .hint { margin: 0; padding: 5px 11px; color: var(--warning); font-size: 10px; }
  .shared-note { border-bottom: 1px solid var(--border); }
  /* The editor is the only thing that grows, so a long script scrolls inside it
     rather than pushing the toolbar off the bottom of the panel. */
  .editor { min-height: 220px; flex: 1 1 auto; overflow: hidden; border-bottom: 1px solid var(--border); }
  .conflict { padding: 9px 11px; border-bottom: 1px solid var(--border); background: color-mix(in srgb, var(--warning) 14%, transparent); }
  .conflict p { margin: 0 0 7px; }
  .choices { display: flex; gap: 6px; }
  .diff { display: grid; gap: 6px; margin-top: 9px; }
  .diff p { margin: 0; color: var(--text-faint); font-size: 10px; }
  .diff h3 { margin: 0 0 3px; color: var(--text-faint); font-size: 9px; letter-spacing: .08em; text-transform: uppercase; }
  .diff pre { max-height: 110px; margin: 0; padding: 6px 8px; overflow: auto; border: 1px solid var(--border); background: var(--canvas); font: 10px/1.45 ui-monospace, monospace; }
  .problems { max-height: 96px; margin: 0; padding: 6px 0; overflow: auto; border-bottom: 1px solid var(--border); list-style: none; }
  .problems button { display: grid; width: 100%; padding: 2px 11px; border: 0; background: none; color: inherit; cursor: pointer; grid-template-columns: 96px 1fr; text-align: left; }
  .problems button:hover:not(:disabled) { background: color-mix(in srgb, var(--text) 7%, transparent); }
  .problems span { overflow: hidden; color: var(--text-faint); font: 9px ui-monospace, monospace; text-overflow: ellipsis; }
  .problems code { color: var(--warning); font: 10px/1.4 ui-monospace, monospace; white-space: pre-wrap; }
  .dropped, .migrated { margin: 0; padding: 7px 11px; border-bottom: 1px solid var(--border); font-size: 10px; }
  .dropped { color: var(--warning); }
  .migrated { color: var(--text-faint); }
  .error, .notice { margin: 0; padding: 7px 11px; border-bottom: 1px solid var(--border); font-size: 10px; }
  .error { color: var(--danger); }
  .notice { color: var(--text-faint); }
  footer { display: flex; align-items: center; padding: 8px 11px; gap: 6px; flex-wrap: wrap; }
  button { height: 23px; padding: 0 9px; border: 1px solid var(--border); border-radius: 3px; background: var(--canvas); color: var(--text); font: 600 10px/1 'Segoe UI', sans-serif; cursor: pointer; }
  button:hover:not(:disabled) { border-color: var(--accent); }
  button:disabled { color: var(--text-faint); cursor: default; }
  button.primary { border-color: var(--accent); background: var(--accent); color: var(--on-accent); }
  button.primary:disabled { border-color: var(--border); background: var(--canvas); color: var(--text-faint); }
  button.ghost { border-color: transparent; }
  button.danger { color: var(--danger); }
  .phase { margin-left: auto; color: var(--text-faint); font-size: 9px; letter-spacing: .06em; text-transform: uppercase; }
  .phase.dirty { color: var(--accent); }
  .phase.conflict { color: var(--warning); }
  .roots, .log { max-height: 120px; margin: 0; padding: 5px 0; overflow: auto; border-top: 1px solid var(--border); background: var(--canvas); list-style: none; }
  .roots li, .log li { padding: 1px 11px; }
  .roots code, .log code { font: 10px/1.45 ui-monospace, monospace; white-space: pre-wrap; overflow-wrap: anywhere; }
  .roots .note code { color: var(--text-faint); font-style: italic; }
  /* On a narrow palette the trade the grip offers no longer exists: there is no
     width at which both the graph and a line of code are readable, so the panel
     takes the lot and the grip has nothing to give. */
  @media (max-width: 620px) { .script-editor { width: 100% !important; max-width: 100%; } .grip { display: none; } }
</style>
