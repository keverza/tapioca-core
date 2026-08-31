<script lang="ts">
  import { NodeResizer, type Node, type NodeProps } from '@xyflow/svelte'
  import type { SchemaNodeData } from '../types'
  import ParameterBrowser from './archicad/ParameterBrowser.svelte'
  import FloatingControlPanel from './controls/FloatingControlPanel.svelte'
  import NodeBody from './NodeBody.svelte'
  import NodeHeader from './NodeHeader.svelte'
  import NodeMenu from './menus/NodeMenu.svelte'
  import { bodyModeFor, categoryColor, nodeDisplayName } from './types/display'
  import { DEFAULT_DISPLAY_STATE } from './types/display'
  import { definitionFromSchema } from './contracts'

  let { id, data, selected }: NodeProps<Node<SchemaNodeData>> = $props()
  let menuOpen = $state(false)
  let inspectorOpen = $state(false)
  let browserOpen = $state(false)

  const definition = $derived(definitionFromSchema(data.schema))
  const mode = $derived(data.executionMode ?? 'enabled')
  const bodyMode = $derived(bodyModeFor(data.schema))
  const viewMode = $derived(data.visual?.viewMode ?? 'standard')
  const portLayout = $derived(data.visual?.portLayout ?? definition.presentation.portLayout)
  const display = $derived(data.visual?.display ?? DEFAULT_DISPLAY_STATE)
  const viewerVisible = $derived(display.nodeViewer)
  const nodeColor = $derived(data.visual?.color ?? categoryColor(data.schema.category))
  const displayName = $derived(nodeDisplayName(data.schema, data.visual?.nickname))
  const status = $derived(data.result?.status ?? 'pending')
  const selectionCount = $derived.by(() => {
    const stored = data.parameters.find((parameter) => parameter.parameterId === 'elements')
    return stored?.value?.itemCount ?? stored?.value?.items?.length ?? 0
  })

  function updateName(name: string): void {
    data.onvisualchange?.(id, { ...data.visual, nickname: name.trim() || undefined })
  }
  function updateColor(color: string): void {
    data.onvisualchange?.(id, { ...data.visual, color })
  }
  function updateVisual(change: Partial<NonNullable<SchemaNodeData['visual']>>): void {
    data.onvisualchange?.(id, { ...data.visual, ...change })
  }
</script>

<article
  class:selected
  class:error={status === 'error'}
  class:inert={status === 'blocked' || status === 'disabled' || status === 'cancelled'}
  class:disabled={mode === 'disabled'}
  class:bypassed={mode === 'bypassed'}
  class:holding={mode === 'holding'}
  class:viewer={bodyMode === 'viewer' || bodyMode === 'parameters+viewer'}
  class:compact={viewMode === 'compact'}
  class:expanded={viewMode === 'expanded'}
  class:primary-display={display.displayRole === 'primary'}
  class:reference-display={display.displayRole === 'reference'}
  style={`--node-color: ${nodeColor}`}
>
  {#if selected && definition.capabilities.resizable && viewMode !== 'compact'}
    <NodeResizer minWidth={definition.presentation.minSize?.width} minHeight={definition.presentation.minSize?.height} maxWidth={definition.presentation.maxSize?.width} maxHeight={definition.presentation.maxSize?.height} />
  {/if}
  <div class="type-rail"></div>
  <NodeHeader schema={data.schema} name={displayName} result={data.result} messages={data.messages} onrename={updateName} onmenu={() => (menuOpen = !menuOpen)} />
  <NodeBody {id} {data} {definition} {bodyMode} {viewMode} {portLayout} {selected} {viewerVisible} onbrowse={() => (browserOpen = true)} />

  {#if menuOpen}
    <NodeMenu
      hasViewer={bodyMode === 'viewer' || bodyMode === 'parameters+viewer'}
      hasReference={data.schema.display === 'selectionSet'}
      oninspect={() => (inspectorOpen = true)}
      onrename={() => (inspectorOpen = true)}
      onview={() => data.schema.display === 'selectionSet' ? (browserOpen = true) : updateVisual({ display: { ...display, nodeViewer: !viewerVisible } })}
      oncontrols={() => (inspectorOpen = true)}
      ontoggleenabled={() => data.onexecutionchange?.(id, mode === 'disabled' ? 'enabled' : 'disabled')}
      onclose={() => (menuOpen = false)}
    />
  {/if}
  {#if browserOpen}
    <ParameterBrowser nodeId={id} count={selectionCount} parameters={data.parameters} busy={data.selectionBusy} onclose={() => (browserOpen = false)} onselectionaction={data.onselectionaction} />
  {/if}
  {#if inspectorOpen}
    <FloatingControlPanel
      schema={data.schema}
      name={displayName}
      color={nodeColor}
      {mode}
      {viewerVisible}
      {viewMode}
      {portLayout}
      {display}
      onname={updateName}
      oncolor={updateColor}
      onmode={(nextMode) => data.onexecutionchange?.(id, nextMode)}
      onviewer={() => updateVisual({ display: { ...display, nodeViewer: !viewerVisible } })}
      onviewmode={(nextMode) => updateVisual({ viewMode: nextMode })}
      onportlayout={(layout) => updateVisual({ portLayout: layout })}
      ondisplay={(nextDisplay) => updateVisual({ display: nextDisplay })}
      onclose={() => (inspectorOpen = false)}
    />
  {/if}
</article>

<style>
  article { position: relative; display: grid; grid-template-rows: auto minmax(0, 1fr); width: 100%; height: 100%; min-height: 44px; overflow: visible; border: 1px solid var(--border); border-radius: 2px; background: var(--surface); color: var(--text); box-shadow: 0 9px 25px rgb(0 0 0 / 32%); }
  article.selected { border-color: var(--node-color); box-shadow: 0 0 0 1px var(--node-color), 0 12px 30px rgb(0 0 0 / 42%); }
  article.error { border-color: var(--danger); }
  article.inert { filter: saturate(.55); }
  article.disabled { border-style: dashed; opacity: .58; }
  article.bypassed { border-style: dashed; }
  article.holding { box-shadow: inset 0 0 0 1px #c9922f; }
  article.compact { min-height: 0; }
  article.primary-display::after { position: absolute; right: -4px; bottom: -4px; width: 7px; height: 7px; border: 1px solid var(--surface); border-radius: 50%; background: var(--node-color); content: ''; }
  article.reference-display::after { position: absolute; right: -4px; bottom: -4px; width: 7px; height: 7px; border: 1px solid var(--node-color); border-radius: 50%; background: var(--surface); content: ''; }
  .type-rail { position: absolute; z-index: 2; inset: 0 auto 0 0; width: 3px; background: var(--node-color); pointer-events: none; }
</style>
