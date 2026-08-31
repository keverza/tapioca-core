<script lang="ts">
  import { Handle, Position } from '@xyflow/svelte'
  import type { PortSchema } from '../types'
  let { port, direction, value }: { port: PortSchema; direction: 'input' | 'output'; value?: string } = $props()
</script>

<div class="port" class:output={direction === 'output'}>
  {#if direction === 'input'}<Handle type="target" position={Position.Left} id={port.portId} />{/if}
  {#if direction === 'output'}<small>{value ?? port.valueType}</small>{/if}
  <span>{port.label}</span>
  {#if direction === 'output'}<Handle type="source" position={Position.Right} id={port.portId} />{/if}
</div>

<style>
  .port { position: relative; display: flex; align-items: center; min-height: 17px; gap: 5px; color: var(--text-muted); font-size: 9px; }
  .port.output { justify-content: flex-end; text-align: right; }
  small { overflow: hidden; max-width: 85px; color: var(--text-faint); font: 7px/1 ui-monospace, monospace; text-overflow: ellipsis; white-space: nowrap; }
  :global(.svelte-flow__handle) { width: 8px; height: 8px; border: 1px solid var(--surface); border-radius: 1px; background: var(--node-color); }
</style>
