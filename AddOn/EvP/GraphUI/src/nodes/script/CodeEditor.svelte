<script lang="ts">
  /**
   * CodeMirror 6, and nothing about scripts.
   *
   * ⚠️ THIS COMPONENT DECIDES NOTHING. It shows text, reports edits, and paints
   * the diagnostics it is handed. Every rule about whether an edit may be saved,
   * what a conflict is and which version wins lives in buffer.ts, where it can be
   * tested without a browser. Keeping the editor dumb is what stops "the rules"
   * from quietly becoming "whatever the widget happened to do".
   */
  import {
    autocompletion,
    closeBrackets,
    closeBracketsKeymap,
    completionKeymap,
    type CompletionContext,
    type CompletionResult,
  } from '@codemirror/autocomplete'
  import { defaultKeymap, history, historyKeymap, indentWithTab } from '@codemirror/commands'
  import { javascript } from '@codemirror/lang-javascript'
  import { python } from '@codemirror/lang-python'
  import {
    HighlightStyle,
    bracketMatching,
    foldGutter,
    foldKeymap,
    indentOnInput,
    indentUnit,
    syntaxHighlighting,
  } from '@codemirror/language'
  import { lintGutter, setDiagnostics } from '@codemirror/lint'
  import { highlightSelectionMatches, searchKeymap } from '@codemirror/search'
  import { Compartment, EditorState } from '@codemirror/state'
  import {
    EditorView,
    drawSelection,
    highlightActiveLine,
    highlightActiveLineGutter,
    highlightSpecialChars,
    keymap,
    lineNumbers,
  } from '@codemirror/view'
  import { tags } from '@lezer/highlight'
  import { untrack } from 'svelte'
  // ⚠️ THE TYPE COMES FROM script.ts, NOT FROM scriptBridge.ts, and that is the
  // same rule as the note at the top: this component must not know there is a
  // bridge. script.ts is the module with no window.EvP in it.
  import type { ScriptCompletionItem } from './script'

  let {
    text,
    language,
    diagnostics = [],
    readOnly = false,
    revealLine = 0,
    complete,
    onchange,
    onsave,
  }: {
    text: string
    language: 'python' | 'javascript'
    /**
     * Completions at a position, from whoever knows how to get them.
     *
     * ⚠️ A CALLBACK, BECAUSE THIS COMPONENT STILL DECIDES NOTHING. It does not
     * know which node it is editing, it must not call the bridge, and it has no
     * business knowing there is a language server at all - see the note at the
     * top. `line` and `character` are handed over exactly as CodeMirror counts
     * them, which is already what LSP means.
     *
     * Absent means no completion beyond CodeMirror's own word list, which is the
     * ordinary state on a machine where the server is not installed.
     */
    complete?: (line: number, character: number, source: string) => Promise<ScriptCompletionItem[]>
    /** 1-based lines, as the header parser and the runtime both report them. */
    diagnostics?: { line: number; message: string; severity?: 'error' | 'warning' }[]
    readOnly?: boolean
    /** Scroll this 1-based line into view once. 0 means "leave the view alone". */
    revealLine?: number
    onchange: (text: string) => void
    /** Ctrl+S inside the editor. The panel owns what saving means. */
    onsave: () => void
  } = $props()

  let host = $state<HTMLDivElement>()
  let view: EditorView | undefined

  /**
   * Asks the owner for completions at the cursor.
   *
   * ⚠️ THE `from` IS THE START OF THE WORD BEING TYPED, NOT THE CURSOR. Return
   * the cursor and CodeMirror inserts the completion AFTER the partial word, so
   * accepting `hypot` at `math.hy|` writes `math.hyhypot`. `matchBefore` finds
   * where the identifier began; after a dot there is no identifier yet and the
   * cursor is correct.
   *
   * ⚠️ AND IT FIRES ON AN EXPLICIT REQUEST OR AFTER A DOT, NOT ON EVERY KEY. Each
   * call is a round trip to a language server; running one per keystroke while
   * somebody types a comment would be constant bridge traffic for a menu nobody
   * asked for. `explicit` is Ctrl+Space.
   */
  async function requestCompletions(context: CompletionContext): Promise<CompletionResult | null> {
    if (complete === undefined) return null
    const word = context.matchBefore(/[\w.]*/)
    const afterDot = context.matchBefore(/\.\s*$/) !== null
    if (!context.explicit && !afterDot && (word === null || word.from === word.to)) return null

    const line = context.state.doc.lineAt(context.pos)
    // Zero-based, and `character` is a UTF-16 offset - which a JavaScript string
    // index already is, so this is the protocol's own unit with no conversion.
    const items = await complete(line.number - 1, context.pos - line.from, context.state.doc.toString())
    if (items.length === 0) return null

    // The identifier being typed, if there is one. `math.hy` -> from at `hy`,
    // because the dot is not part of the word being completed.
    const identifier = context.matchBefore(/[A-Za-z_]\w*$/)
    return {
      from: identifier === null ? context.pos : identifier.from,
      options: items.map((item) => ({
        label: item.label,
        apply: item.insertText,
        type: item.kind === '' ? undefined : item.kind,
        detail: item.detail === '' ? undefined : item.detail,
        info: item.documentation === '' ? undefined : item.documentation,
      })),
      // ⚠️ THE SERVER IS RE-ASKED AS THE USER KEEPS TYPING. Pyright returns the
      // members of the expression BEFORE the dot; filtering that list locally is
      // right for `hy` -> `hypot`, but the moment another dot or a bracket is
      // typed the expression itself changed and the old list is about a
      // different object.
      validFor: /^\w*$/,
    }
  }

  const languageSlot = new Compartment()
  const readOnlySlot = new Compartment()

  /*
   * The token colours are CSS VARIABLES, not literals, so the editor follows the
   * palette's own light/dark switch instead of being a rectangle that stays dark
   * when everything around it turns white. The fallbacks matter: this component
   * can be mounted before styles.css has defined a variable.
   */
  const highlight = HighlightStyle.define([
    { tag: tags.keyword, color: 'var(--code-keyword)' },
    { tag: [tags.controlKeyword, tags.moduleKeyword], color: 'var(--code-keyword)' },
    { tag: [tags.function(tags.variableName), tags.function(tags.propertyName)], color: 'var(--code-function)' },
    { tag: [tags.string, tags.special(tags.string)], color: 'var(--code-string)' },
    { tag: [tags.number, tags.bool, tags.null], color: 'var(--code-number)' },
    { tag: [tags.comment, tags.lineComment, tags.blockComment], color: 'var(--code-comment)', fontStyle: 'italic' },
    { tag: [tags.typeName, tags.className], color: 'var(--code-type)' },
    { tag: tags.operator, color: 'var(--code-operator)' },
    { tag: tags.propertyName, color: 'var(--code-property)' },
    { tag: tags.invalid, color: 'var(--code-invalid)' },
  ])

  const theme = EditorView.theme({
    '&': { height: '100%', color: 'var(--text)', backgroundColor: 'var(--canvas)', fontSize: '12px' },
    '.cm-scroller': { fontFamily: 'ui-monospace, Consolas, monospace', lineHeight: '1.5' },
    '.cm-gutters': { backgroundColor: 'var(--canvas)', color: 'var(--text-faint)', border: 'none' },
    '.cm-activeLineGutter': { backgroundColor: 'transparent', color: 'var(--text)' },
    '.cm-activeLine': { backgroundColor: 'color-mix(in srgb, var(--text) 5%, transparent)' },
    '.cm-content': { caretColor: 'var(--text)' },
    '&.cm-focused .cm-cursor': { borderLeftColor: 'var(--text)' },
    '&.cm-focused .cm-selectionBackground, .cm-selectionBackground, ::selection': {
      backgroundColor: 'color-mix(in srgb, var(--accent) 30%, transparent)',
    },
    '.cm-selectionMatch': { backgroundColor: 'color-mix(in srgb, var(--accent) 18%, transparent)' },
    '.cm-panels': { backgroundColor: 'var(--surface)', color: 'var(--text)' },
    '.cm-panels input, .cm-panels button': {
      border: '1px solid var(--border)', borderRadius: '3px', background: 'var(--canvas)', color: 'var(--text)',
    },
    '.cm-tooltip': { border: '1px solid var(--border)', backgroundColor: 'var(--surface)', color: 'var(--text)' },
  })

  function modeFor(name: 'python' | 'javascript') {
    return name === 'javascript' ? javascript() : python()
  }

  /*
   * ⚠️ THE VIEW IS BUILT INSIDE untrack, AND WITHOUT THAT THIS COMPONENT IS
   * UNUSABLE. An $effect subscribes to every reactive value it reads, so the
   * plain version of this - reading `text` to seed the document - tears the
   * editor down and builds a new one on every keystroke, losing the cursor, the
   * selection and the undo history each time. Only `host` may be a dependency:
   * the view is created once per mount and kept in step by the effects below.
   */
  $effect(() => {
    if (host === undefined) return
    const parent = host
    const created = untrack(() => new EditorView({
      parent,
      state: EditorState.create({
        doc: text,
        extensions: [
          lineNumbers(),
          highlightActiveLineGutter(),
          highlightActiveLine(),
          highlightSpecialChars(),
          drawSelection(),
          history(),
          foldGutter(),
          indentOnInput(),
          // Four spaces, and the same for both languages. A Python script whose
          // indentation is a tab where the rest of the file uses spaces does not
          // run, and the one place that is most likely to happen is a quick fix
          // typed into an embedded editor.
          indentUnit.of('    '),
          bracketMatching(),
          closeBrackets(),
          autocompletion({
            // The language server's answers come FIRST and CodeMirror's own
            // word list stays: on a machine with no server installed the word
            // list is all there is, and losing it there would make the editor
            // worse than it was before code intelligence existed.
            override: complete === undefined ? undefined : [requestCompletions],
            activateOnTyping: true,
          }),
          highlightSelectionMatches(),
          syntaxHighlighting(highlight),
          lintGutter(),
          languageSlot.of(modeFor(language)),
          readOnlySlot.of(EditorState.readOnly.of(readOnly)),
          theme,
          keymap.of([
            /*
             * ⚠️ CTRL+S IS BOUND HERE AND SWALLOWED. Unbound, it reaches the
             * host - which in a WebView is the browser's own Save Page As, over
             * a palette, from a user who meant to save their script. `preventDefault`
             * is what the `true` return does for us.
             */
            { key: 'Mod-s', preventDefault: true, run: () => { onsave(); return true } },
            ...closeBracketsKeymap,
            ...defaultKeymap,
            ...searchKeymap,
            ...historyKeymap,
            ...foldKeymap,
            ...completionKeymap,
            // Tab indents rather than moving focus. Inside a code editor that is
            // what Tab means; the escape hatch for keyboard users is Escape then
            // Tab, which indentWithTab leaves intact.
            indentWithTab,
          ]),
          EditorView.updateListener.of((update) => {
            if (update.docChanged) onchange(update.state.doc.toString())
          }),
        ],
      }),
    }))
    view = created
    return () => {
      view = undefined
      created.destroy()
    }
  })

  /*
   * ⚠️ THE DOCUMENT IS REPLACED ONLY WHEN IT ACTUALLY DIFFERS, and never in
   * response to the editor's own typing. Dispatching the prop back into the view
   * on every keystroke would reset the selection and the undo history at
   * character speed. The comparison is against the live document rather than a
   * remembered copy, so an external reload - taking the file after a conflict -
   * lands, and an echo of the user's own edit does not.
   */
  $effect(() => {
    const current = view
    const next = text
    if (current === undefined) return
    if (current.state.doc.toString() === next) return
    current.dispatch({ changes: { from: 0, to: current.state.doc.length, insert: next } })
  })

  $effect(() => {
    view?.dispatch({ effects: languageSlot.reconfigure(modeFor(language)) })
  })

  $effect(() => {
    view?.dispatch({ effects: readOnlySlot.reconfigure(EditorState.readOnly.of(readOnly)) })
  })

  /*
   * Diagnostics come from the header parser and from the last run, both of which
   * report a 1-based line and nothing narrower. A whole-line marker is therefore
   * the honest rendering - inventing a column range would point confidently at a
   * character the reporter never named.
   */
  $effect(() => {
    const current = view
    const items = diagnostics
    if (current === undefined) return
    const total = current.state.doc.lines
    current.dispatch(
      setDiagnostics(
        current.state,
        items
          .filter((item) => item.line >= 1 && item.line <= total)
          .map((item) => {
            const line = current.state.doc.line(item.line)
            return { from: line.from, to: line.to, severity: item.severity ?? 'error', message: item.message }
          }),
      ),
    )
  })

  $effect(() => {
    const current = view
    const target = revealLine
    if (current === undefined || target < 1 || target > current.state.doc.lines) return
    const line = current.state.doc.line(target)
    current.dispatch({ selection: { anchor: line.from }, scrollIntoView: true })
    current.focus()
  })
</script>

<!--
  `nodrag` and `nowheel` keep SvelteFlow's hands off: without them a drag inside
  the editor pans the canvas and a scroll zooms it, which makes selecting a
  paragraph of code move the graph out from under the panel.
-->
<div class="code nodrag nowheel" bind:this={host}></div>

<style>
  .code { min-height: 0; height: 100%; overflow: hidden; background: var(--canvas); }
  .code :global(.cm-editor) { height: 100%; }
  .code :global(.cm-editor.cm-focused) { outline: none; }
</style>
