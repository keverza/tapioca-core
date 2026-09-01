<script lang="ts">
  import { Handle, Position } from '@xyflow/svelte'
  import type { PortConnectionState, PortDefinition, PortLayout } from './types/port'
  import type { ComponentMessage } from './types/diagnostics'
  import { describeStructure, portColor, type PortStructure } from './types/display'

  let { nodeId, port, direction, layout, value, structure = 'item', connection, messages = [], oncontextmenu }: { nodeId: string; port: PortDefinition; direction: 'input' | 'output'; layout: PortLayout; value?: string; structure?: PortStructure; connection?: PortConnectionState; messages?: ComponentMessage[]; oncontextmenu?: (event: MouseEvent, portId: string, direction: 'input' | 'output') => void } = $props()
  const position = $derived(layout === 'vertical' ? (direction === 'input' ? Position.Top : Position.Bottom) : (direction === 'input' ? Position.Left : Position.Right))
  /**
   * The nub's colour is the VALUE TYPE's, set as a custom property on this row
   * so the handle - which the flow library renders as our child - picks it up by
   * inheritance. Styling the handle any other way would need a global rule per
   * type, and the handle's class list is not ours to extend.
   */
  const color = $derived(portColor(port.valueType))

  /**
   * The port draws no menu of its own. It asks the editor to open THE context
   * menu, the same one a node, an edge and the canvas open - a port that drew
   * its own put a second panel, in a second size, on top of the node's.
   *
   * Propagation stops here so the node does not also answer the same press.
   */
  function requestMenu(event: MouseEvent): void {
    if (oncontextmenu === undefined) return
    event.preventDefault()
    event.stopPropagation()
    oncontextmenu(event, port.portId, direction)
  }
</script>

<div class="port" class:vertical={layout === 'vertical'} class:output={direction === 'output'} class:connected={connection?.connected} class:list={structure === 'list'} class:tree={structure === 'tree'} style={`--port-color: ${color}`} role="button" tabindex="0" oncontextmenu={requestMenu} onkeydown={(event) => { if (event.key === 'ContextMenu' || (event.shiftKey && event.key === 'F10')) requestMenu(event as unknown as MouseEvent) }}>
  {#if direction === 'input'}<Handle type="target" {position} id={port.portId} />{/if}
  {#if direction === 'output'}<small>{value ?? port.valueType}</small>{/if}
  <span>{port.nickname ?? port.label}</span>
  {#if direction === 'output'}<Handle type="source" {position} id={port.portId} />{/if}
  <aside class="tooltip nodrag">
    <strong>{port.nickname ?? port.label}</strong><span><i style={`background: ${color}`}></i>{port.valueType}{port.required ? ' / required' : ''}</span>
    <span>{describeStructure(structure)}</span>
    <span>{connection?.connected ? `${connection.connectionCount} connection${connection.connectionCount === 1 ? '' : 's'}` : 'Not connected'}</span>
    {#each connection?.peerLabels ?? [] as peer}<code>{peer}</code>{/each}
    {#each messages as message}<em class={message.severity}>{message.code}: {message.title}</em>{/each}
  </aside>
</div>

<style>
  /*
   * The flow library centres a handle on the EDGE OF THIS BOX - left: 0 with a
   * -50% translate - so where the box ends is where the nub sits. That is the
   * whole fix: the row now spans the node edge to edge and its own PADDING
   * holds the label clear, where before the row was inset by the body's padding
   * and dragged the nub inward on top of the text. Every container that draws
   * ports therefore contributes no horizontal padding of its own.
   */
  .port { position: relative; display: flex; align-items: center; min-height: 17px; padding-left: 14px; gap: 5px; color: var(--text-muted); font-size: 9px; }
  .port.output { justify-content: flex-end; padding-right: 14px; padding-left: 0; text-align: right; }
  /* Stacked layout: the nub is on the top or bottom edge, so the clearance is
     vertical and the label re-centres. */
  .port.vertical { justify-content: center; padding: 11px 4px 0; text-align: center; }
  .port.vertical.output { padding: 0 4px 11px; }
  .port.connected > span { color: var(--text); }
  small { overflow: hidden; max-width: 85px; color: var(--text-faint); font: 7px/1 ui-monospace, monospace; text-overflow: ellipsis; white-space: nowrap; }
  .tooltip { position: absolute; z-index: 18; top: calc(100% + 5px); left: 0; display: none; min-width: 150px; padding: 7px; border: 1px solid var(--border); background: var(--surface-raised); box-shadow: 0 9px 22px rgb(0 0 0 / 38%); text-align: left; }
  .output .tooltip { right: 0; left: auto; }
  .port:hover .tooltip { display: grid; gap: 3px; }
  .tooltip strong { color: var(--text); font-size: 9px; }
  .tooltip span, .tooltip code { color: var(--text-faint); font: 7px/1.3 ui-monospace, monospace; }
  .tooltip em { color: #c9922f; font: 7px/1.3 ui-monospace, monospace; font-style: normal; }
  .tooltip em.error { color: var(--danger); }

  .tooltip i { display: inline-block; width: 7px; height: 7px; margin-right: 5px; border-radius: 2px; vertical-align: middle; }
  /*
   * STRUCTURE, drawn as Grasshopper draws it: an item is the bare nub, a list
   * adds a second outline around it, and a tree makes that outline dashed.
   *
   * The ring is a ::before on the handle rather than a box-shadow because a
   * shadow cannot be dashed, and ::after is already spoken for - it is the
   * enlarged hit region below. The rules are scoped to this row and reach the
   * handle globally, since the flow library renders that element and its class
   * list is not ours to extend.
   */
  .port.list :global(.svelte-flow__handle)::before,
  .port.tree :global(.svelte-flow__handle)::before {
    position: absolute; z-index: 1; inset: -3px; border: 1px solid var(--port-color); border-radius: 4px; content: '';
  }
  .port.tree :global(.svelte-flow__handle)::before { border-style: dashed; }

  /* The nub takes the PORT's colour and falls back to the node's, so a port
     whose type has no entry still draws rather than going transparent. */
  :global(.svelte-flow__handle) { z-index: 3; box-sizing: border-box; width: 11px; height: 11px; border: 1px solid var(--surface); border-radius: 2px; background: var(--port-color, var(--node-color)); }
  /*
   * The drawn nub stays small; what the pointer has to hit does not. The
   * pseudo-element is part of the handle's own hit region, so a press anywhere
   * in the 23 px square starts the connection - which is the difference
   * between "aim at it" and "click near it".
   */
  :global(.svelte-flow__handle)::after { position: absolute; inset: -6px; border-radius: 4px; content: ''; }
  :global(.svelte-flow__handle.connectingfrom), :global(.svelte-flow__handle.connectingto) { box-shadow: 0 0 0 3px color-mix(in srgb, var(--port-color, var(--node-color)) 45%, transparent); }
</style>
