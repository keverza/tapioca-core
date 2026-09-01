<script lang="ts">
  import type { ExecutionMode, NodeTypeSchema } from '../../types'
  import type { PortConnectionState } from '../types/port'
  import type { DisplayState } from '../types/display'
  import type { NodeViewMode } from '../types/node'
  import type { PortLayout } from '../types/port'

  let {
    schema,
    name,
    color,
    mode,
    viewerVisible,
    viewMode,
    portLayout,
    display,
    nodeId,
    connections = [],
    onname,
    oncolor,
    onmode,
    onviewer,
    onviewmode,
    onportlayout,
    ondisplay,
    oncopyreference,
    onpastereference,
    onclose,
  }: {
    schema: NodeTypeSchema
    name: string
    color: string
    mode: ExecutionMode
    viewerVisible: boolean
    viewMode: NodeViewMode
    portLayout: PortLayout
    display: DisplayState
    nodeId: string
    connections?: PortConnectionState[]
    onname: (name: string) => void
    oncolor: (color: string) => void
    onmode: (mode: ExecutionMode) => void
    onviewer: () => void
    onviewmode: (mode: NodeViewMode) => void
    onportlayout: (layout: PortLayout) => void
    ondisplay: (display: DisplayState) => void
    oncopyreference?: (nodeId: string, portId: string, direction: 'input' | 'output') => void
    onpastereference?: (nodeId: string, portId: string) => void
    onclose: () => void
  } = $props()

  const colors = ['#d58b4b', '#65a9b8', '#7da76a', '#a580bd', '#c56f72', '#d2c66c', '#8194aa', '#d9d9d9']
  const canBypass = $derived((schema.bypassMappings?.length ?? 0) > 0)
  function inputConnection(portId: string): PortConnectionState | undefined {
    return connections.find((connection) => connection.direction === 'input' && connection.portId === portId)
  }
</script>

<aside class="inspector nodrag nowheel" aria-label="Node inspector">
  <header><span>Node inspector</span><button type="button" onclick={onclose}>Close</button></header>

  <fieldset>
    <legend>Name</legend>
    <input value={name} oninput={(event) => onname(event.currentTarget.value)} aria-label="Custom node name" />
    <small>{schema.label} / {schema.category}</small>
  </fieldset>

  <fieldset>
    <legend>Presentation</legend>
    <div class="action-grid thirds">
      {#each ['compact', 'standard', 'expanded'] as item}<button class:active={viewMode === item} type="button" onclick={() => onviewmode(item as NodeViewMode)}>{item}</button>{/each}
    </div>
    <div class="state-grid two">
      <button class:active={portLayout === 'horizontal'} type="button" onclick={() => onportlayout('horizontal')}>Left / right</button>
      <button class:active={portLayout === 'vertical'} type="button" onclick={() => onportlayout('vertical')}>Top / bottom</button>
    </div>
  </fieldset>

  <fieldset>
    <legend>Display</legend>
    <div class="action-grid thirds">
      {#each ['primary', 'reference', 'none'] as role}<button class:active={display.displayRole === role} type="button" onclick={() => ondisplay({ ...display, displayRole: role as DisplayState['displayRole'] })}>{role}</button>{/each}
    </div>
    <div class="state-grid">
      <button class:active={display.nodeViewer} type="button" onclick={() => ondisplay({ ...display, nodeViewer: !display.nodeViewer })}>Node viewer</button>
      <button type="button" disabled title="Canvas preview renderer is not connected to native display state">Canvas</button>
      <button type="button" disabled title="Archicad overlay requires a native display transaction">Archicad</button>
    </div>
    <select value={display.output ?? ''} onchange={(event) => ondisplay({ ...display, output: event.currentTarget.value || undefined })} aria-label="Display output">
      <option value="">Automatic output</option>
      {#each schema.outputs as output}<option value={output.portId}>{output.label}</option>{/each}
    </select>
    <select value={display.style.representation} onchange={(event) => ondisplay({ ...display, style: { ...display.style, representation: event.currentTarget.value as DisplayState['style']['representation'] } })} aria-label="Display representation">
      {#each ['default', 'shaded', 'wireframe', 'ghosted', 'points', 'bounds'] as representation}<option value={representation}>{representation}</option>{/each}
    </select>
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
    <legend>References</legend>
    {#each schema.inputs as input (input.portId)}
      {@const connection = inputConnection(input.portId)}
      <div class="reference-row">
        <span>{input.label}</span>
        <code title={connection?.peerLabels?.[0]}>{connection?.peerLabels?.[0] ?? 'local'}</code>
        <button type="button" disabled={!connection?.connected || oncopyreference === undefined} title={connection?.connected ? 'Copy the upstream output reference' : 'This input has no upstream reference'} onclick={() => oncopyreference?.(nodeId, input.portId, 'input')}>Copy</button>
        <button type="button" disabled={onpastereference === undefined} onclick={() => onpastereference?.(nodeId, input.portId)}>Paste</button>
      </div>
    {/each}
    {#each schema.outputs as output (output.portId)}
      <div class="reference-row output-reference">
        <span>{output.label}</span><code>{output.valueType}</code>
        <button type="button" disabled={oncopyreference === undefined} onclick={() => oncopyreference?.(nodeId, output.portId, 'output')}>Copy</button>
        <span></span>
      </div>
    {/each}
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
  .state-grid.two { grid-template-columns: 1fr 1fr; }
  .state-grid button, .action-grid button { border-radius: 0; }
  button.active { border-color: var(--node-color); background: color-mix(in srgb, var(--node-color) 46%, var(--surface)); color: var(--text); }
  .swatches { display: flex; height: 22px; }
  .swatches button { width: 26px; height: 22px; padding: 0; border-radius: 0; background: var(--swatch); }
  .swatches button.active { outline: 2px solid var(--text); outline-offset: -3px; }
  .swatches input { width: 27px; height: 22px; padding: 0; border-radius: 0; }
  .action-grid { display: grid; grid-template-columns: repeat(4, 1fr); }
  .reference-row { display: grid; grid-template-columns: minmax(48px, .8fr) minmax(60px, 1fr) 42px 42px; align-items: center; gap: 3px; }
  .reference-row > span { overflow: hidden; color: var(--text-muted); font-size: 8px; text-overflow: ellipsis; white-space: nowrap; }
  .reference-row code { overflow: hidden; color: var(--text-faint); font: 8px/1 ui-monospace, monospace; text-overflow: ellipsis; white-space: nowrap; }
  .reference-row button { height: 22px; padding: 0 4px; }
  .action-grid.flow { grid-template-columns: repeat(3, 1fr); }
  .action-grid.thirds { grid-template-columns: repeat(3, 1fr); }
  select { width: 100%; height: 27px; padding: 0 6px; font-size: 8px; }
  aside > p { margin: 2px 5px 3px; color: var(--text-faint); font-size: 8px; line-height: 1.4; }
</style>
