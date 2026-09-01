<script lang="ts">
  import type { NodeBodyMode, NodeDefinition, NodeViewMode } from './types/node'
  import type { PortLayout } from './types/port'
  import type { SchemaNodeData, SelectionAction } from '../types'
  import type { PortReference } from './types/portReference'
  import ElementReference from './archicad/ElementReference.svelte'
  import NodeControls from './NodeControls.svelte'
  import NodePort from './NodePort.svelte'
  import NodeViewer from './NodeViewer.svelte'
  import { portStructure } from './types/display'

  let { id, data, definition, bodyMode, viewMode, portLayout, viewerVisible, onbrowse }: { id: string; data: SchemaNodeData; definition: NodeDefinition; bodyMode: NodeBodyMode; viewMode: NodeViewMode; portLayout: PortLayout; viewerVisible: boolean; onbrowse: () => void } = $props()
  function connectReference(reference: PortReference, portId: string): void {
    data.onportreference?.(reference, { nodeId: id, portId })
  }
  function portMenu(event: MouseEvent, portId: string, direction: 'input' | 'output'): void {
    data.onportcontextmenu?.(event, { nodeId: id, portId, direction })
  }
  let viewerActive = $state(false)
  const selectionCount = $derived.by(() => {
    const stored = data.parameters.find((parameter) => parameter.parameterId === 'elements')
    return stored?.value?.itemCount ?? stored?.value?.items?.length ?? 0
  })
  const panelLines = $derived(data.result?.outputs?.find((output) => output.portId === 'text')?.text?.split('\n') ?? [])
  function outputText(portId: string): string | undefined { return data.result?.outputs?.find((output) => output.portId === portId)?.summary }
  function outputValue(portId: string) { return data.result?.outputs?.find((output) => output.portId === portId)?.value }
  const actions: { action: SelectionAction; label: string }[] = [
    { action: 'update', label: 'Update' }, { action: 'add', label: 'Add' }, { action: 'remove', label: 'Remove' }, { action: 'reselect', label: 'Reselect' }, { action: 'clear', label: 'Clear' },
  ]
  function toggleViewer(): void { viewerActive = !viewerActive }
</script>

<div class="body" class:compact={viewMode === 'compact'} class:expanded={viewMode === 'expanded'} class:vertical={portLayout === 'vertical'}>
{#if viewMode !== 'compact' && (bodyMode === 'parameters' || bodyMode === 'parameters+viewer')}
  <NodeControls nodeId={id} {definition} parameters={data.parameters} outputs={data.result?.outputs} layout={portLayout} connections={data.portConnections ?? []} messages={data.messages} attributeRows={data.attributeRows} onparameter={data.onparameterchange} onreference={data.onportreference === undefined ? undefined : connectReference} onportmenu={data.onportcontextmenu === undefined ? undefined : portMenu} onrequestoptions={data.onrequestoptions} />
{:else}
  <section class="ports">
    <div>{#each definition.inputs as input}<NodePort nodeId={id} port={input} direction="input" layout={portLayout} structure={portStructure(input)} connection={data.portConnections?.find((item) => item.portId === input.portId && item.direction === 'input')} messages={data.messages?.filter((message) => message.portId === input.portId)} oncontextmenu={data.onportcontextmenu === undefined ? undefined : portMenu} />{/each}</div>
    <div>{#each definition.outputs as output}<NodePort nodeId={id} port={output} direction="output" layout={portLayout} value={outputText(output.portId)} structure={portStructure(output, outputValue(output.portId))} connection={data.portConnections?.find((item) => item.portId === output.portId && item.direction === 'output')} messages={data.messages?.filter((message) => message.portId === output.portId)} oncontextmenu={data.onportcontextmenu === undefined ? undefined : portMenu} />{/each}</div>
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

<!--
  The preview is driven by the node's DISPLAY FLAG, not by selection. Having to
  click a node to see what it produced makes the preview a thing you go and
  fetch; the flag makes it a property of the node, which is what a Watch is for.
  Turn it off - on the ring's Viewer segment, or in the inspector's Display
  section - and the node stops drawing one.
-->
{#if viewMode !== 'compact' && viewerVisible && (bodyMode === 'viewer' || bodyMode === 'parameters+viewer')}
  <section class="viewer" class:active={viewerActive}><NodeViewer result={data.result} active={viewerActive} onactive={toggleViewer} /></section>
{/if}
</div>

<style>
  .body { display: flex; min-height: 0; flex-direction: column; }
  /* No padding on the axis the handles sit on, so a port row reaches the node's
     edge and its nub lands on the border rather than over the label. The row
     carries its own clearance instead; see NodePort. */
  .ports { display: flex; justify-content: space-between; min-height: 25px; padding: 7px 0 8px; gap: 10px; }
  .ports > div { display: grid; min-width: 0; flex: 1 1 0; align-content: start; gap: 5px; }
  .vertical .ports { display: grid; padding: 0; gap: 8px; }
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
  /*
   * A DEFINITE height, and that is the whole point rather than a taste in
   * sizing. The preview canvas sizes itself to this box; while the box was
   * sized by its content, each rendered frame made the box taller, which made
   * the next frame taller still - the node grew without bound from the moment
   * selecting it mounted the canvas. Fixing the basis breaks the loop; flex-grow
   * still lets a node the user has RESIZED hand the viewer the extra room,
   * because that height comes from the node and not from the canvas.
   */
  .viewer { height: 168px; min-height: 0; flex: 1 1 auto; padding: 0 9px 8px 11px; }
  .expanded .viewer { height: 248px; }
  .viewer:not(.active) :global(canvas) { pointer-events: none; }
</style>
