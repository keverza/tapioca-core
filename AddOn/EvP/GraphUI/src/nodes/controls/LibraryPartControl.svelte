<script lang="ts">
  /**
   * A library-part parameter: the chosen object's name, and a button that opens
   * the browser.
   *
   * ⚠️ A BUTTON, NOT A DROPDOWN, AND THAT IS THE WHOLE DECISION. A stock
   * Archicad library is several thousand parts in a deep folder tree; a
   * `<select>` of four thousand names is a control that technically works and
   * cannot be used, which is why the palette's evp.LibraryPart is a button onto a
   * three-pane dialog and why this is too. See LibraryBrowser.svelte.
   *
   * ⚠️ THE VALUE IS THE PALETTE'S OWN JSON, and the button shows the `name` out
   * of it. Storing the label alone would lose the unID, and a graph that named
   * its object only by a document name is a graph that silently picks a different
   * part the day a second library loads one with the same name.
   */
  import { decodeSelection } from './libraryParts'

  let {
    value,
    label,
    disabled = false,
    onbrowse,
  }: {
    value: string
    label: string
    disabled?: boolean
    /**
     * Asks the NODE to open the browser.
     *
     * ⚠️ THE PANEL IS NOT RENDERED HERE, and that is a positioning rule this
     * repository has already paid for once. A panel asking for
     * `left: calc(100% + 10px)` inside a control cell resolves 100% against the
     * CONTROL - a hundred-odd pixels - and lands on top of the node it belongs
     * to. MasterNode's .panel-anchor exists precisely so `100%` means the
     * node's width; see its comment. Every other panel goes through it, and so
     * does this one now.
     */
    onbrowse?: () => void
  } = $props()

  const selection = $derived(decodeSelection(value))
</script>

<div class="library-part">
  <button
    type="button"
    class="chosen"
    {disabled}
    aria-label={`${label}: ${selection?.name ?? 'no object chosen'}`}
    title={selection === undefined
      ? 'Choose an object from the loaded libraries'
      : `${selection.name}\n${selection.file}${selection.location === '' ? '' : `\n${selection.location}`}`}
    onclick={onbrowse}
  >
    <span class="name" class:empty={selection === undefined}>{selection?.name ?? 'Choose an object...'}</span>
    <span class="caret" aria-hidden="true">...</span>
  </button>
  {#if selection !== undefined && selection.unID === ''}
    <!--
      ⚠️ SAID RATHER THAN HIDDEN. A value carrying a name and no unID came from
      somewhere that did not record the identity - a hand-edited file, or an
      older graph - and it will resolve to whichever part currently answers to
      that name. That is exactly the failure the unID exists to prevent, so the
      control says the value is weaker than it looks instead of rendering it
      identically to a sound one.
    -->
    <small>chosen by name only - re-pick to record its unique ID</small>
  {/if}
</div>

<style>
  .library-part { display: grid; min-width: 0; gap: 2px; }
  .chosen { display: flex; width: 100%; height: 21px; align-items: center; padding: 0 5px; border: 1px solid var(--border); border-radius: 2px; background: var(--canvas); color: var(--text); cursor: pointer; gap: 5px; }
  .chosen:hover:not(:disabled) { border-color: var(--node-color); }
  .chosen:disabled { cursor: default; opacity: .6; }
  .name { overflow: hidden; flex: 1 1 auto; font: 9px/1 'Segoe UI', sans-serif; text-align: left; text-overflow: ellipsis; white-space: nowrap; }
  .name.empty { color: var(--text-faint); font-style: italic; }
  .caret { color: var(--text-faint); font: 9px/1 ui-monospace, monospace; }
  small { color: var(--warning); font: 8px/1.3 'Segoe UI', sans-serif; }
</style>
