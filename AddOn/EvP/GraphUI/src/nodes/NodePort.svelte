<script lang="ts">
  import { Handle, Position } from '@xyflow/svelte'
  import InputMenu from './menus/InputMenu.svelte'
  import OutputMenu from './menus/OutputMenu.svelte'
  import type { PortConnectionState, PortDefinition, PortLayout } from './types/port'
  import type { ComponentMessage } from './types/diagnostics'
  let { nodeId, port, direction, layout, value, connection, messages = [] }: { nodeId: string; port: PortDefinition; direction: 'input' | 'output'; layout: PortLayout; value?: string; connection?: PortConnectionState; messages?: ComponentMessage[] } = $props()
  let menuOpen = $state(false)
  const position = $derived(layout === 'vertical' ? (direction === 'input' ? Position.Top : Position.Bottom) : (direction === 'input' ? Position.Left : Position.Right))
  const reference = $derived(JSON.stringify({ kind: 'nodePort', nodeId, portId: port.portId, direction, valueType: port.valueType }))

  function copyReference(): void {
    void navigator.clipboard?.writeText(reference)
  }
</script>

<div class="port" class:output={direction === 'output'} class:connected={connection?.connected} class:menu-open={menuOpen} role="button" tabindex="0" oncontextmenu={(event) => { event.preventDefault(); menuOpen = !menuOpen }} onkeydown={(event) => { if (event.key === 'ContextMenu' || (event.shiftKey && event.key === 'F10')) { event.preventDefault(); menuOpen = !menuOpen } }}>
  {#if direction === 'input'}<Handle type="target" {position} id={port.portId} />{/if}
  {#if direction === 'output'}<small>{value ?? port.valueType}</small>{/if}
  <span>{port.nickname ?? port.label}</span>
  {#if direction === 'output'}<Handle type="source" {position} id={port.portId} />{/if}
  <aside class="tooltip nodrag">
    <strong>{port.nickname ?? port.label}</strong><span>{port.valueType}{port.required ? ' / required' : ''}</span>
    <span>{connection?.connected ? `${connection.connectionCount} connection${connection.connectionCount === 1 ? '' : 's'}` : 'Not connected'}</span>
    {#each connection?.peerLabels ?? [] as peer}<code>{peer}</code>{/each}
    {#each messages as message}<em class={message.severity}>{message.code}: {message.title}</em>{/each}
  </aside>
  {#if menuOpen}
    {#if direction === 'input'}<InputMenu capabilities={port.capabilities} transforms={connection?.transforms} oncopy={copyReference} onclose={() => (menuOpen = false)} />
    {:else}<OutputMenu capabilities={port.capabilities} oncopy={copyReference} onclose={() => (menuOpen = false)} />{/if}
  {/if}
</div>

<style>
  .port { position: relative; display: flex; align-items: center; min-height: 17px; gap: 5px; color: var(--text-muted); font-size: 9px; }
  .port.output { justify-content: flex-end; text-align: right; }
  .port.connected > span { color: var(--text); }
  small { overflow: hidden; max-width: 85px; color: var(--text-faint); font: 7px/1 ui-monospace, monospace; text-overflow: ellipsis; white-space: nowrap; }
  .tooltip { position: absolute; z-index: 18; top: calc(100% + 5px); left: 0; display: none; min-width: 150px; padding: 7px; border: 1px solid var(--border); background: var(--surface-raised); box-shadow: 0 9px 22px rgb(0 0 0 / 38%); text-align: left; }
  .output .tooltip { right: 0; left: auto; }
  .port:hover .tooltip { display: grid; gap: 3px; }
  .port.menu-open .tooltip { display: none; }
  .tooltip strong { color: var(--text); font-size: 9px; }
  .tooltip span, .tooltip code { color: var(--text-faint); font: 7px/1.3 ui-monospace, monospace; }
  .tooltip em { color: #c9922f; font: 7px/1.3 ui-monospace, monospace; font-style: normal; }
  .tooltip em.error { color: var(--danger); }
  :global(.svelte-flow__handle) { width: 8px; height: 8px; border: 1px solid var(--surface); border-radius: 1px; background: var(--node-color); }
</style>
