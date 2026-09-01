<script lang="ts">
  /**
   * True or false, as a switch.
   *
   * A real `<input type="checkbox">` under the drawn track, not a styled div
   * with a click handler: that is what makes it reachable by Tab, operable with
   * Space, and announced as a checkbox. The track and knob are the label's
   * pseudo-elements.
   */
  import { booleanText } from './widgets'

  let {
    value,
    label,
    disabled = false,
    oncommit,
  }: { value: boolean; label: string; disabled?: boolean; oncommit: (text: string) => void } = $props()
</script>

<label class="toggle nodrag" class:on={value} class:disabled>
  <input
    type="checkbox"
    checked={value}
    {disabled}
    aria-label={label}
    onchange={(event) => oncommit(booleanText(event.currentTarget.checked))}
  />
  <span class="track"><span class="knob"></span></span>
  <span class="state">{value ? 'True' : 'False'}</span>
</label>

<style>
  .toggle { display: flex; align-items: center; gap: 6px; cursor: pointer; }
  .toggle.disabled { cursor: default; opacity: .5; }
  /* Visually hidden, not display:none - a hidden input takes no focus and the
     switch would stop being keyboard-operable. */
  input { position: absolute; width: 1px; height: 1px; opacity: 0; }
  .track { position: relative; width: 26px; height: 13px; flex: none; border: 1px solid var(--border); border-radius: 7px; background: var(--canvas); transition: background-color 120ms, border-color 120ms; }
  .knob { position: absolute; top: 1px; left: 1px; width: 9px; height: 9px; border-radius: 50%; background: var(--text-faint); transition: transform 120ms, background-color 120ms; }
  .on .track { border-color: var(--node-color); background: color-mix(in srgb, var(--node-color) 30%, var(--canvas)); }
  .on .knob { background: var(--node-color); transform: translateX(13px); }
  input:focus-visible + .track { outline: 1px solid var(--node-color); outline-offset: 1px; }
  .state { color: var(--text-muted); font: 9px/1 ui-monospace, monospace; }
  .on .state { color: var(--text); }

  @media (prefers-reduced-motion: reduce) {
    .track, .knob { transition: none; }
  }
</style>
