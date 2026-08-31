<script lang="ts">
  import type { Node, NodeProps } from '@xyflow/svelte'
  import type { SchemaNodeData } from '../types'
  import ParameterBrowser from './archicad/ParameterBrowser.svelte'
  import FloatingControlPanel from './controls/FloatingControlPanel.svelte'
  import NodeBody from './NodeBody.svelte'
  import NodeHeader from './NodeHeader.svelte'
  import NodeMenu from './menus/NodeMenu.svelte'
  import { bodyModeFor, categoryColor, nodeDisplayName } from './types/display'

  let { id, data, selected }: NodeProps<Node<SchemaNodeData>> = $props()
  let menuOpen = $state(false)
  let inspectorOpen = $state(false)
  let browserOpen = $state(false)
  let viewerVisible = $state(true)

  const mode = $derived(data.executionMode ?? 'enabled')
  const bodyMode = $derived(bodyModeFor(data.schema))
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
</script>

<article
  class:selected
  class:error={status === 'error'}
  class:inert={status === 'blocked' || status === 'disabled' || status === 'cancelled'}
  class:disabled={mode === 'disabled'}
  class:bypassed={mode === 'bypassed'}
  class:holding={mode === 'holding'}
  class:viewer={bodyMode === 'viewer' || bodyMode === 'parameters+viewer'}
  style={`--node-color: ${nodeColor}`}
>
  <div class="type-rail"></div>
  <NodeHeader schema={data.schema} name={displayName} result={data.result} onrename={updateName} onmenu={() => (menuOpen = !menuOpen)} />
  <NodeBody {id} {data} {bodyMode} {selected} {viewerVisible} onbrowse={() => (browserOpen = true)} />

  {#if menuOpen}
    <NodeMenu
      hasViewer={bodyMode === 'viewer' || bodyMode === 'parameters+viewer'}
      hasReference={data.schema.display === 'selectionSet'}
      oninspect={() => (inspectorOpen = true)}
      onrename={() => (inspectorOpen = true)}
      onview={() => data.schema.display === 'selectionSet' ? (browserOpen = true) : (viewerVisible = !viewerVisible)}
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
      onname={updateName}
      oncolor={updateColor}
      onmode={(nextMode) => data.onexecutionchange?.(id, nextMode)}
      onviewer={() => (viewerVisible = !viewerVisible)}
      onclose={() => (inspectorOpen = false)}
    />
  {/if}
</article>

<style>
  article { position: relative; width: 248px; overflow: visible; border: 1px solid var(--border); border-radius: 2px; background: var(--surface); color: var(--text); box-shadow: 0 9px 25px rgb(0 0 0 / 32%); }
  article.viewer { width: 292px; }
  article.selected { border-color: var(--node-color); box-shadow: 0 0 0 1px var(--node-color), 0 12px 30px rgb(0 0 0 / 42%); }
  article.error { border-color: var(--danger); }
  article.inert { filter: saturate(.55); }
  article.disabled { border-style: dashed; opacity: .58; }
  article.bypassed { border-style: dashed; }
  article.holding { box-shadow: inset 0 0 0 1px #c9922f; }
  .type-rail { position: absolute; z-index: 2; inset: 0 auto 0 0; width: 3px; background: var(--node-color); pointer-events: none; }
</style>
