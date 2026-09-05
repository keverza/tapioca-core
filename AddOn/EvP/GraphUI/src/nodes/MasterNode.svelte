<script lang="ts">
  import { NodeResizer, type Node, type NodeProps } from '@xyflow/svelte'
  import type { SchemaNodeData } from '../types'
  import ParameterBrowser from './archicad/ParameterBrowser.svelte'
  import FloatingControlPanel from './controls/FloatingControlPanel.svelte'
  import LibraryBrowser from './controls/LibraryBrowser.svelte'
  import { decodeSelection, encodeSelection } from './controls/libraryParts'
  import type { LibraryPartRow } from '../types'
  import NodeBody from './NodeBody.svelte'
  import NodeHeader from './NodeHeader.svelte'
  import NodeMenu from './menus/NodeMenu.svelte'
  import { bodyModeFor, categoryColor, nodeDisplayName, previewTargetOf } from './types/display'
  import { DEFAULT_DISPLAY_STATE } from './types/display'
  import { definitionFromSchema } from './contracts'
  import { clearRequest, pendingRequest } from './renameRequest.svelte'

  let { id, data, selected }: NodeProps<Node<SchemaNodeData>> = $props()
  let menuOpen = $state(false)
  let header = $state<ReturnType<typeof NodeHeader>>()
  // Measured so the quick menu's hole can be sized to clear this node; a ring
  // with a fixed hole covers the thing it is a menu for.
  let nodeWidth = $state(0)
  let nodeHeight = $state(0)
  let inspectorOpen = $state(false)
  let browserOpen = $state(false)
  /**
   * Which library-part parameter has the object browser open, or '' for none.
   *
   * Held HERE rather than in the control for the reason .panel-anchor exists:
   * a panel positioned against a control cell lands on top of the node.
   */
  let libraryParameterId = $state('')
  /**
   * Whether there is anything to browse.
   *
   * Derived ONCE and shared by the header arrow and the ring menu, because two
   * copies of this predicate is how a node ends up with an arrow that opens an
   * empty panel while its menu correctly greys the same action out.
   */
  const hasBrowser = $derived(
    data.schema.display === 'selectionSet' || (data.result?.outputs?.length ?? 0) > 0 || data.parameters.length > 0,
  )

  const definition = $derived(definitionFromSchema(data.schema))
  const mode = $derived(data.executionMode ?? 'enabled')
  const bodyMode = $derived(bodyModeFor(data.schema))
  const viewMode = $derived(data.visual?.viewMode ?? 'standard')
  const portLayout = $derived(data.visual?.portLayout ?? definition.presentation.portLayout)
  const display = $derived(data.visual?.display ?? DEFAULT_DISPLAY_STATE)
  /**
   * ⚠️ ONE SWITCH FOR BOTH HALVES WHEN THE NODE HAS ONE. A node carrying a
   * previewTarget parameter says where it draws, and that parameter is
   * authoritative over the browser-only eye toggle: two controls for one fact
   * would let them disagree, and "showing nothing" would then have two causes
   * that look identical. Nodes with no such parameter - Watch - keep the eye.
   */
  const previewTarget = $derived(previewTargetOf(data.schema, data.parameters))
  const viewerVisible = $derived(previewTarget === undefined ? display.nodeViewer : previewTarget !== 'archicad')
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

  /**
   * MIDDLE-CLICK opens the quick menu.
   *
   * Handled on pointerdown and stopped here rather than on `auxclick`, because
   * the middle button is also the canvas pan gesture: letting the press reach
   * the viewport would start a pan under the menu that just opened. Stopping it
   * on the node means middle-drag still pans everywhere else.
   */
  // The canvas context menu asked for this node by id; the field and the menu
  // are both here.
  $effect(() => {
    const kind = pendingRequest(id)
    if (kind === undefined) return
    clearRequest()
    if (kind === 'rename') header?.begin()
    else menuOpen = true
  })

  function pointerDown(event: PointerEvent): void {
    if (event.button !== 1) return
    event.preventDefault()
    event.stopPropagation()
    // Opens, rather than toggles. Closing is a press on the node in the hole,
    // Escape, or choosing something - and a toggle here would fight the
    // outside-press that has already run on this very press.
    menuOpen = true
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
  onpointerdown={pointerDown}
  bind:clientWidth={nodeWidth}
  bind:clientHeight={nodeHeight}
>
  {#if selected && definition.capabilities.resizable && viewMode !== 'compact'}
    <NodeResizer minWidth={definition.presentation.minSize?.width} minHeight={definition.presentation.minSize?.height} maxWidth={definition.presentation.maxSize?.width} maxHeight={definition.presentation.maxSize?.height} />
  {/if}
  <div class="type-rail"></div>
  <NodeHeader bind:this={header} schema={data.schema} name={displayName} result={data.result} messages={data.messages} canBrowse={hasBrowser} onbrowse={() => (browserOpen = true)} onrename={updateName} />
  <NodeBody
    {id}
    {data}
    {definition}
    {bodyMode}
    {viewMode}
    {portLayout}
    {viewerVisible}
    onbrowse={() => (browserOpen = true)}
    onbrowselibrary={(parameterId) => {
      libraryParameterId = libraryParameterId === parameterId ? '' : parameterId
      if (libraryParameterId !== '') data.onrequestlibrary?.()
    }}
  />

  {#if menuOpen}
    <NodeMenu
      {mode}
      {nodeWidth}
      {nodeHeight}
      hasViewer={bodyMode === 'viewer' || bodyMode === 'parameters+viewer'}
      hasReference={data.schema.display === 'selectionSet'}
      {viewerVisible}
      canBypass={definition.capabilities.bypass}
      canHold={definition.capabilities.freeze}
      oninspect={() => (inspectorOpen = true)}
      onrename={() => header?.begin()}
      onbrowse={() => (browserOpen = true)}
      {hasBrowser}
      onview={() => data.schema.display === 'selectionSet' ? (browserOpen = true) : updateVisual({ display: { ...display, nodeViewer: !viewerVisible } })}
      oncontrols={() => (inspectorOpen = true)}
      ontoggleenabled={() => data.onexecutionchange?.(id, mode === 'disabled' ? 'enabled' : 'disabled')}
      onbypass={() => data.onexecutionchange?.(id, mode === 'bypassed' ? 'enabled' : 'bypassed')}
      onhold={() => data.onexecutionchange?.(id, mode === 'holding' ? 'enabled' : 'holding')}
      onclose={() => (menuOpen = false)}
    />
  {/if}
  {#if libraryParameterId !== ''}
    <div class="panel-anchor">
      <LibraryBrowser
        catalog={data.libraryCatalog}
        selectedName={decodeSelection(
          data.parameters.find((parameter) => parameter.parameterId === libraryParameterId)?.value?.text,
        )?.name ?? ''}
        onchoose={(part: LibraryPartRow) => {
          data.onparameterchange?.(id, libraryParameterId, 'string', encodeSelection(part))
          libraryParameterId = ''
        }}
        onclose={() => (libraryParameterId = '')}
        onrequest={data.onrequestlibrary}
        onpreview={data.onlibrarypreview}
      />
    </div>
  {/if}
  {#if browserOpen}
    <div class="panel-anchor">
      <ParameterBrowser
        nodeId={id}
        count={selectionCount}
        parameters={data.parameters}
        outputs={data.result?.outputs}
        outputPorts={data.schema.outputs}
        isSelectionSet={data.schema.display === 'selectionSet'}
        busy={data.selectionBusy}
        elementGroups={data.elementGroups}
        ondescribeelements={data.ondescribeelements}
        promoted={data.promotedSettings}
        onpromote={(target) => data.onpromotesetting?.(id, target)}
        onsettingmenu={(target) => data.onsettingmenu?.(id, target)}
        onclose={() => (browserOpen = false)}
        onselectionaction={data.onselectionaction}
        oncopyreference={data.oncopyportreference}
      />
    </div>
  {/if}
  {#if inspectorOpen}
    <div class="panel-anchor">
    <FloatingControlPanel
      schema={data.schema}
      nodeId={id}
      connections={data.portConnections}
      elementGroups={data.elementGroups}
      ondescribeelements={data.ondescribeelements}
      onsettingmenu={(target) => data.onsettingmenu?.(id, target)}
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
      oncopyreference={data.oncopyportreference}
      onpastereference={data.onpasteportreference}
      onclose={() => (inspectorOpen = false)}
    />
    </div>
  {/if}
</article>

<style>
  article { position: relative; display: grid; grid-template-rows: auto minmax(0, 1fr); width: 100%; height: 100%; min-height: 44px; overflow: visible; border: 1px solid var(--border); border-radius: 2px; background: var(--surface); color: var(--text); box-shadow: 0 9px 25px rgb(0 0 0 / 32%); }
  article.selected { border-color: var(--node-color); box-shadow: 0 0 0 1px var(--node-color), 0 12px 30px rgb(0 0 0 / 42%); }
  article.error { border-color: var(--danger); }
  article.inert { filter: saturate(.55); }
  article.disabled { border-style: dashed; opacity: .58; }
  article.bypassed { border-style: dashed; }
  article.holding { box-shadow: inset 0 0 0 1px var(--warning); }
  article.compact { min-height: 0; }
  article.primary-display::after { position: absolute; right: -4px; bottom: -4px; width: 7px; height: 7px; border: 1px solid var(--surface); border-radius: 50%; background: var(--node-color); content: ''; }
  article.reference-display::after { position: absolute; right: -4px; bottom: -4px; width: 7px; height: 7px; border: 1px solid var(--node-color); border-radius: 50%; background: var(--surface); content: ''; }
  .type-rail { position: absolute; z-index: 2; inset: 0 auto 0 0; width: 3px; background: var(--node-color); pointer-events: none; }
  /*
   * The node's own box, so a panel that asks for `left: calc(100% + 10px)`
   * lands to the RIGHT OF THE NODE. A zero-sized anchor made 100% mean 0 and
   * dropped every panel on top of the node it belongs to. It takes no pointer
   * events of its own so it cannot shadow the node underneath.
   *
   * These panels do NOT close on an outside press. A panel you are working out
   * of has to survive a click on the canvas or on another node - only menus,
   * which are transient by definition, put themselves away.
   */
  .panel-anchor { position: absolute; z-index: 14; inset: 0; pointer-events: none; }
  .panel-anchor > :global(*) { pointer-events: auto; }
</style>
