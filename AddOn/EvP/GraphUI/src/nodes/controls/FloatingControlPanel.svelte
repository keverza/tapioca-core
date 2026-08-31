<script lang="ts">
  import type { ExecutionMode, NodeTypeSchema } from '../../types'

  let {
    schema,
    name,
    color,
    mode,
    viewerVisible,
    onname,
    oncolor,
    onmode,
    onviewer,
    onclose,
  }: {
    schema: NodeTypeSchema
    name: string
    color: string
    mode: ExecutionMode
    viewerVisible: boolean
    onname: (name: string) => void
    oncolor: (color: string) => void
    onmode: (mode: ExecutionMode) => void
    onviewer: () => void
    onclose: () => void
  } = $props()

  const colors = ['#d58b4b', '#65a9b8', '#7da76a', '#a580bd', '#c56f72', '#d2c66c', '#8194aa', '#d9d9d9']
  const canBypass = $derived((schema.bypassMappings?.length ?? 0) > 0)
</script>

<aside class="inspector nodrag nowheel" aria-label="Node inspector">
  <header><span>Node inspector</span><button type="button" onclick={onclose}>Close</button></header>

  <fieldset>
    <legend>Name</legend>
    <input value={name} oninput={(event) => onname(event.currentTarget.value)} aria-label="Custom node name" />
    <small>{schema.label} / {schema.category}</small>
  </fieldset>

  <fieldset>
    <legend>State</legend>
    <div class="state-grid">
      <button class:active={mode === 'enabled'} type="button" onclick={() => onmode('enabled')}>Enabled</button>
      <button class:active={mode === 'disabled'} type="button" onclick={() => onmode('disabled')}>Disabled</button>
      <button class:active={viewerVisible} type="button" onclick={onviewer}>Visible</button>
    </div>
    <div class="swatches" aria-label="Node color">
      {#each colors as swatch}<button class:active={color === swatch} type="button" style={`--swatch: ${swatch}`} aria-label={`Set color ${swatch}`} onclick={() => oncolor(swatch)}></button>{/each}
      <input type="color" value={color} oninput={(event) => oncolor(event.currentTarget.value)} aria-label="Custom color" />
    </div>
  </fieldset>

  <fieldset>
    <legend>Data</legend>
    <div class="action-grid">
      <button type="button" disabled title="Port-scoped operation">Disconnect</button>
      <button type="button" disabled title="Port-scoped operation">Internalise</button>
      <button type="button" disabled title="Port-scoped operation">Promote</button>
      <button type="button" disabled title="Requires a runtime data inspector">Inspect</button>
    </div>
  </fieldset>

  <fieldset>
    <legend>Flow</legend>
    <div class="action-grid flow">
      <button class:active={mode === 'bypassed'} type="button" disabled={!canBypass} title={canBypass ? 'Forward the declared bypass input' : 'No bypass mapping declared'} onclick={() => onmode('bypassed')}>Bypass</button>
      <button class:active={mode === 'holding'} type="button" disabled={!schema.holdCapable} title={schema.holdCapable ? 'Hold the last value' : 'Node is not hold-capable'} onclick={() => onmode('holding')}>Hold</button>
      <button type="button" disabled title="Input transform pipeline belongs to a port">Modifiers</button>
    </div>
  </fieldset>

  <p>{schema.description}</p>
</aside>

<style>
  aside { position: absolute; z-index: 14; top: 0; left: calc(100% + 10px); width: 276px; padding: 8px; border: 1px solid var(--border); border-top: 2px solid var(--node-color); border-radius: 2px; background: var(--surface); box-shadow: 0 18px 44px rgb(0 0 0 / 48%); color: var(--text); }
  header { display: flex; align-items: center; justify-content: space-between; margin: -8px -8px 5px; padding: 7px 8px; border-bottom: 1px solid var(--border); background: var(--surface-raised); }
  header span { color: var(--text-muted); font: 600 9px/1 ui-monospace, monospace; text-transform: uppercase; }
  button { height: 25px; padding: 0 7px; font-size: 8px; }
  fieldset { display: grid; margin: 0; padding: 8px 5px 7px; border: 0; border-top: 1px solid var(--border); gap: 6px; }
  fieldset:first-of-type { border-top: 0; }
  legend { padding: 0 7px; color: var(--text-faint); font: 8px/1 ui-monospace, monospace; text-align: center; }
  fieldset > input { width: 100%; height: 27px; padding: 0 7px; }
  fieldset small { color: var(--text-faint); font: 7px/1 ui-monospace, monospace; }
  .state-grid { display: grid; grid-template-columns: 1fr 1fr 1fr; }
  .state-grid button, .action-grid button { border-radius: 0; }
  button.active { border-color: var(--node-color); background: color-mix(in srgb, var(--node-color) 46%, var(--surface)); color: var(--text); }
  .swatches { display: flex; height: 22px; }
  .swatches button { width: 26px; height: 22px; padding: 0; border-radius: 0; background: var(--swatch); }
  .swatches button.active { outline: 2px solid var(--text); outline-offset: -3px; }
  .swatches input { width: 27px; height: 22px; padding: 0; border-radius: 0; }
  .action-grid { display: grid; grid-template-columns: repeat(4, 1fr); }
  .action-grid.flow { grid-template-columns: repeat(3, 1fr); }
  aside > p { margin: 2px 5px 3px; color: var(--text-faint); font-size: 8px; line-height: 1.4; }
</style>
