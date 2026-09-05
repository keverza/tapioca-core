<script lang="ts">
  /**
   * A line of text, and - with `swatch` - a colour.
   *
   * The colour widget carries no new value type: it is a canonical hex string,
   * so it round-trips through the same string parameter and the same setParam
   * encoding as any other text. The swatch is a second input bound to the same
   * value, which is what makes the picker available without making the typed
   * spelling unavailable.
   */
  import { parsePortReference, type PortReference } from '../types/portReference'

  let {
    value,
    label,
    placeholder = '',
    disabled = false,
    swatch = false,
    oncommit,
    onreference,
  }: {
    value: string
    label: string
    placeholder?: string
    disabled?: boolean
    swatch?: boolean
    oncommit: (text: string) => void
    onreference?: (reference: PortReference) => void
  } = $props()

  const HEX = /^#[0-9a-fA-F]{6}$/
  // ⚠️ NOT A THEME TOKEN, AND MUST NOT BECOME ONE. This is the swatch shown for
  // a value that is not a colour - it stands for "unreadable", so it has to look
  // the same in both themes or the swatch would report the theme rather than the
  // value.
  const colour = $derived(HEX.test(value.trim()) ? value.trim() : '#808080')

  function commit(next: string): void {
    if (next.trim() === value.trim()) return
    oncommit(next)
  }

  /**
   * A paste is either a value or a WIRE - the port menu puts its own reference
   * format on the clipboard, so a pasted reference becomes a connection request
   * and never reaches the field as text.
   */
  function handlePaste(event: ClipboardEvent): void {
    if (onreference === undefined) return
    const reference = parsePortReference(event.clipboardData?.getData('text/plain') ?? '')
    if (reference === undefined) return
    event.preventDefault()
    onreference(reference)
  }
</script>

<div class="text" class:with-swatch={swatch}>
  {#if swatch}
    <input class="swatch nodrag" type="color" value={colour} {disabled} aria-label={`${label} colour`} oninput={(event) => commit(event.currentTarget.value)} />
  {/if}
  <input
    class="field nodrag"
    type="text"
    {value}
    {placeholder}
    {disabled}
    aria-label={`${label} value`}
    onpaste={handlePaste}
    onblur={(event) => commit(event.currentTarget.value)}
    onkeydown={(event) => { if (event.key === 'Enter') event.currentTarget.blur() }}
  />
</div>

<style>
  .text { display: grid; align-items: center; }
  .with-swatch { grid-template-columns: 17px minmax(0, 1fr); gap: 3px; }
  .field { width: 100%; min-width: 0; height: 19px; padding: 0 5px; border: 1px solid transparent; border-radius: 2px; background: var(--canvas); color: var(--text); font: 9px/1.2 ui-monospace, monospace; }
  .field:hover:not(:disabled) { border-color: var(--border); }
  .field:focus { border-color: var(--node-color); outline: none; }
  .field::placeholder { color: var(--text-faint); }
  .field:disabled { color: var(--text-faint); }
  .swatch { width: 17px; height: 17px; padding: 0; border: 1px solid var(--border); border-radius: 2px; background: var(--canvas); cursor: pointer; }
  .swatch:disabled { cursor: default; opacity: .5; }
</style>
