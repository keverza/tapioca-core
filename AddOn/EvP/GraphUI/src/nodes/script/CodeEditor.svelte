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
  import { autocompletion, closeBrackets, closeBracketsKeymap, completionKeymap } from '@codemirror/autocomplete'
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

  let {
    text,
    language,
    diagnostics = [],
    readOnly = false,
    revealLine = 0,
    onchange,
    onsave,
  }: {
    text: string
    language: 'python' | 'javascript'
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

  const languageSlot = new Compartment()
  const readOnlySlot = new Compartment()

  /*
   * The token colours are CSS VARIABLES, not literals, so the editor follows the
   * palette's own light/dark switch instead of being a rectangle that stays dark
   * when everything around it turns white. The fallbacks matter: this component
   * can be mounted before styles.css has defined a variable.
   */
  const highlight = HighlightStyle.define([
    { tag: tags.keyword, color: 'var(--code-keyword, #c678dd)' },
    { tag: [tags.controlKeyword, tags.moduleKeyword], color: 'var(--code-keyword, #c678dd)' },
    { tag: [tags.function(tags.variableName), tags.function(tags.propertyName)], color: 'var(--code-function, #61afef)' },
    { tag: [tags.string, tags.special(tags.string)], color: 'var(--code-string, #98c379)' },
    { tag: [tags.number, tags.bool, tags.null], color: 'var(--code-number, #d19a66)' },
    { tag: [tags.comment, tags.lineComment, tags.blockComment], color: 'var(--code-comment, #7d8799)', fontStyle: 'italic' },
    { tag: [tags.typeName, tags.className], color: 'var(--code-type, #e5c07b)' },
    { tag: tags.operator, color: 'var(--code-operator, #56b6c2)' },
    { tag: tags.propertyName, color: 'var(--code-property, #d0d5db)' },
    { tag: tags.invalid, color: 'var(--code-invalid, #e06c75)' },
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
      backgroundColor: 'color-mix(in srgb, var(--accent, #4c8cd8) 30%, transparent)',
    },
    '.cm-selectionMatch': { backgroundColor: 'color-mix(in srgb, var(--accent, #4c8cd8) 18%, transparent)' },
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
          autocompletion(),
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
