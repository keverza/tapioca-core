<script lang="ts">
  import type { NodeBodyMode, NodeDefinition, NodeViewMode } from './types/node'
  import type { PortLayout } from './types/port'
  import type { SchemaNodeData, SelectionAction } from '../types'
  import ElementReference from './archicad/ElementReference.svelte'
  import NodeControls from './NodeControls.svelte'
  import NodePort from './NodePort.svelte'
  import NodeViewer from './NodeViewer.svelte'

  let { id, data, definition, bodyMode, viewMode, portLayout, selected, viewerVisible, onbrowse }: { id: string; data: SchemaNodeData; definition: NodeDefinition; bodyMode: NodeBodyMode; viewMode: NodeViewMode; portLayout: PortLayout; selected: boolean; viewerVisible: boolean; onbrowse: () => void } = $props()
  let viewerActive = $state(false)
  const selectionCount = $derived.by(() => {
    const stored = data.parameters.find((parameter) => parameter.parameterId === 'elements')
    return stored?.value?.itemCount ?? stored?.value?.items?.length ?? 0
  })
  const panelLines = $derived(data.result?.outputs?.find((output) => output.portId === 'text')?.text?.split('\n') ?? [])
  function outputText(portId: string): string | undefined { return data.result?.outputs?.find((output) => output.portId === portId)?.summary }
  const actions: { action: SelectionAction; label: string }[] = [
    { action: 'update', label: 'Update' }, { action: 'add', label: 'Add' }, { action: 'remove', label: 'Remove' }, { action: 'reselect', label: 'Reselect' }, { action: 'clear', label: 'Clear' },
  ]
  function toggleViewer(): void { viewerActive = !viewerActive }
</script>

<div class="body" class:compact={viewMode === 'compact'} class:expanded={viewMode === 'expanded'} class:vertical={portLayout === 'vertical'}>
{#if viewMode !== 'compact' && (bodyMode === 'parameters' || bodyMode === 'parameters+viewer')}
  <NodeControls nodeId={id} {definition} parameters={data.parameters} outputs={data.result?.outputs} layout={portLayout} connections={data.portConnections ?? []} messages={data.messages} />
{:else}
  <section class="ports">
    <div>{#each definition.inputs as input}<NodePort nodeId={id} port={input} direction="input" layout={portLayout} connection={data.portConnections?.find((item) => item.portId === input.portId && item.direction === 'input')} messages={data.messages?.filter((message) => message.portId === input.portId)} />{/each}</div>
    <div>{#each definition.outputs as output}<NodePort nodeId={id} port={output} direction="output" layout={portLayout} value={outputText(output.portId)} connection={data.portConnections?.find((item) => item.portId === output.portId && item.direction === 'output')} messages={data.messages?.filter((message) => message.portId === output.portId)} />{/each}</div>
  </section>
{/if}

{#if viewMode !== 'compact' && data.schema.display === 'selectionSet'}
  <section class="selection nodrag">
    <ElementReference count={selectionCount} onclick={onbrowse} />
    <div>{#each actions as item}<button type="button" disabled={data.selectionBusy || !data.onselectionaction} onclick={() => data.onselectionaction?.(id, item.action)}>{item.label}</button>{/each}</div>
  </section>
{:else if viewMode !== 'compact' && data.schema.display === 'text'}
  <section class="text nodrag nowheel">{#if panelLines.length === 0}<p>{data.result ? '(nothing)' : 'Not evaluated'}</p>{:else}<ol>{#each panelLines as line, index}<li><span>{index + 1}</span><code>{line}</code></li>{/each}</ol>{/if}</section>
{/if}

{#if viewMode !== 'compact' && viewerVisible && (bodyMode === 'viewer' || bodyMode === 'parameters+viewer')}
  <section class="viewer" class:active={viewerActive}>{#if selected}<NodeViewer result={data.result} active={viewerActive} onactive={toggleViewer} />{:else}<div class="placeholder">Select node to load preview</div>{/if}</section>
{/if}
</div>

<style>
  .body { display: flex; min-height: 0; flex-direction: column; }
  .ports { display: flex; justify-content: space-between; min-height: 25px; padding: 7px 10px 8px; gap: 10px; }
  .ports > div { display: grid; align-content: start; gap: 5px; }
  .vertical .ports { display: grid; gap: 8px; }
  .vertical .ports > div { display: flex; justify-content: space-around; gap: 10px; }
  .selection { padding: 0 9px 9px 11px; }
  .selection > div { display: grid; grid-template-columns: repeat(5, 1fr); margin-top: 4px; gap: 2px; }
  .selection > div button { height: 22px; padding: 0 2px; font-size: 7px; }
  .text { max-height: 170px; margin: 0 9px 9px 11px; overflow: auto; border: 1px solid var(--border); background: var(--canvas); }
  .text p { margin: 0; padding: 12px; color: var(--text-faint); font-size: 9px; text-align: center; }
  .text ol { margin: 0; padding: 4px 0; list-style: none; }
  .text li { display: grid; grid-template-columns: 22px 1fr; padding: 2px 6px; }
  .text li span { color: var(--text-faint); font: 8px/1.4 ui-monospace, monospace; text-align: right; }
  .text code { padding-left: 7px; color: var(--text); font: 9px/1.4 ui-monospace, monospace; white-space: pre-wrap; }
  .viewer { min-height: 150px; flex: 1; padding: 0 9px 8px 11px; }
  .expanded .viewer :global(section) { height: 230px; }
  .viewer:not(.active) :global(canvas) { pointer-events: none; }
  .placeholder { display: grid; height: 120px; place-items: center; border: 1px solid var(--border); background: radial-gradient(circle at 50% 40%, var(--surface-raised), var(--canvas) 70%); color: var(--text-faint); font: 8px/1 ui-monospace, monospace; }
</style>
