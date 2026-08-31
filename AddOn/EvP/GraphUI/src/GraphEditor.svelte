<script lang="ts">
  import {
    Background,
    BackgroundVariant,
    Controls,
    MiniMap,
    SvelteFlow,
    useSvelteFlow,
    type ColorMode,
    type Connection,
    type Edge,
    type IsValidConnection,
    type Node,
    type NodeTypes,
    type XYPosition,
  } from '@xyflow/svelte'
  import '@xyflow/svelte/dist/style.css'
  import { onMount } from 'svelte'
  import ApplicationMenu from './ApplicationMenu.svelte'
  import { callTapioca, isNativeBridgeAvailable } from './bridge'
  import ComponentPicker from './ComponentPicker.svelte'
  import ContextMenu from './ContextMenu.svelte'
  import {
    detailLevelForZoom,
    initialTheme,
    isCatalogConnectionValid,
    NODE_DRAG_MIME,
    type DetailLevel,
    type ThemeMode,
  } from './editor'
  import PerformancePanel from './PerformancePanel.svelte'
  import type { DiagnosticMode } from './performance'
  import SchemaNode from './SchemaNode.svelte'
  import type {
    GraphEdgeRecord,
    GraphState,
    NodeResultRecord,
    NodeTypeSchema,
    PositionStore,
    SchemaNodeData,
  } from './types'

  const nodeTypes: NodeTypes = { schema: SchemaNode }
  const positions: PositionStore = new Map()
  const SNAP_GRID: [number, number] = [16, 16]
  const { fitView, screenToFlowPosition } = useSvelteFlow<Node<SchemaNodeData>, Edge>()

  type ContextTarget =
    | { kind: 'pane'; x: number; y: number }
    | { kind: 'node'; x: number; y: number; node: Node<SchemaNodeData> }
    | { kind: 'edge'; x: number; y: number; edge: Edge }

  let nodes = $state.raw<Node<SchemaNodeData>[]>([])
  let edges = $state.raw<Edge[]>([])
  let catalog = $state.raw<NodeTypeSchema[]>([])
  let results = $state.raw<NodeResultRecord[]>([])
  let revision = $state(0)
  let busy = $state(false)
  let message = $state('Connecting to the native graph runtime...')
  let failed = $state(false)
  let performanceOpen = $state(false)
  let diagnosticMode = $state<DiagnosticMode>('flow')
  let snapEnabled = $state(true)
  let pickerOpen = $state(true)
  let theme = $state<ThemeMode>('system')
  let detailLevel = $state<DetailLevel>('normal')
  let contextTarget = $state<ContextTarget | null>(null)
  let nativeConnected = $state(isNativeBridgeAvailable())
  let marker = $state<HTMLDivElement>()
  let canvas = $state<HTMLElement>()

  function edgeId(edge: GraphEdgeRecord): string {
    return [edge.sourceNode, edge.sourcePort, edge.targetNode, edge.targetPort]
      .map(encodeURIComponent)
      .join('--')
  }

  function positionFor(nodeId: string, index: number) {
    const retained = positions.get(nodeId)
    if (retained !== undefined) return retained
    const position = { x: 80 + (index % 4) * 290, y: 80 + Math.floor(index / 4) * 230 }
    positions.set(nodeId, position)
    return position
  }

  function applyState(state: GraphState): void {
    revision = state.revision
    const schemas = new Map(catalog.map((item) => [item.nodeType, item]))
    const resultMap = new Map(results.map((item) => [item.nodeId, item]))
    nodes = state.nodes.map((node, index) => ({
      id: node.nodeId,
      type: 'schema',
      position: positionFor(node.nodeId, index),
      data: {
        schema: schemas.get(node.nodeType) ?? {
          nodeType: node.nodeType,
          label: node.nodeType,
          category: 'Unknown',
          description: 'The native catalog did not describe this node type.',
          executionDomain: 'worker',
          display: 'ports',
          inputs: [],
          outputs: [],
          parameters: [],
        },
        parameters: node.parameters,
        result: resultMap.get(node.nodeId),
      },
    }))
    edges = state.edges.map((edge) => ({
      id: edgeId(edge),
      source: edge.sourceNode,
      sourceHandle: edge.sourcePort,
      target: edge.targetNode,
      targetHandle: edge.targetPort,
      type: 'smoothstep',
    }))
  }

  async function refreshResults(): Promise<void> {
    const response = await callTapioca<{ results: NodeResultRecord[] }>('Tapioca.GraphGetNodeResults')
    results = response.results
  }

  async function reloadState(): Promise<void> {
    applyState(await callTapioca<GraphState>('Tapioca.GraphGetState'))
  }

  async function initialize(): Promise<void> {
    busy = true
    failed = false
    try {
      if (!nativeConnected) {
        initializeStandaloneFixture()
        message = 'Standalone diagnostic fixture / no native bridge'
        return
      }
      const response = await callTapioca<{ nodeTypes: NodeTypeSchema[] }>('Tapioca.GraphGetNodeTypes')
      catalog = response.nodeTypes
      // The runtime keeps results across an editor reload, so pick them up on
      // open rather than showing an empty graph until the next evaluation.
      await refreshResults()
      await reloadState()
      message = `${catalog.length} native node types / revision ${revision}`
    } catch (error) {
      failed = true
      message = error instanceof Error ? error.message : String(error)
    } finally {
      busy = false
    }
  }

  function initializeStandaloneFixture(): void {
    catalog = [
      {
        nodeType: 'diagnostic.source',
        label: 'Source',
        category: 'Diagnostic',
        description: 'Browser-only source used for host comparison.',
        executionDomain: 'browser',
        display: 'ports',
        inputs: [],
        outputs: [{ portId: 'value', label: 'Value', valueType: 'number' }],
        parameters: [],
      },
      {
        nodeType: 'diagnostic.sink',
        label: 'Sink',
        category: 'Diagnostic',
        description: 'Browser-only sink used for host comparison.',
        executionDomain: 'browser',
        display: 'ports',
        inputs: [{ portId: 'value', label: 'Value', valueType: 'number' }],
        outputs: [],
        parameters: [],
      },
    ]
    applyState({
      revision: 0,
      nodes: [
        { nodeId: 'diagnostic-source', nodeType: 'diagnostic.source', parameters: [] },
        { nodeId: 'diagnostic-sink', nodeType: 'diagnostic.sink', parameters: [] },
      ],
      edges: [
        {
          sourceNode: 'diagnostic-source',
          sourcePort: 'value',
          targetNode: 'diagnostic-sink',
          targetPort: 'value',
        },
      ],
    })
  }

  async function addNode(nodeType: string, requestedPosition?: XYPosition): Promise<void> {
    if (nodeType === '') return
    const position = requestedPosition ?? positionAtViewportCenter()
    const nodeId = `${nodeType}-${Date.now().toString(36)}`
    if (!nativeConnected) {
      const schema = catalog.find((item) => item.nodeType === nodeType)
      if (schema === undefined) return
      positions.set(nodeId, position)
      nodes = [
        ...nodes,
        {
          id: nodeId,
          type: 'schema',
          position,
          data: { schema, parameters: [] },
        },
      ]
      message = `Added browser-only ${nodeId}`
      return
    }
    busy = true
    failed = false
    try {
      positions.set(nodeId, position)
      const params: Record<string, unknown> = {
        editKind: 'addNode',
        nodeId,
        nodeType,
      }
      if (nodeType === 'number') params.numberValue = 0
      await callTapioca('Tapioca.GraphApplyEdit', params)
      await reloadState()
      message = `Added ${nodeId} / revision ${revision}`
    } catch (error) {
      positions.delete(nodeId)
      failed = true
      message = error instanceof Error ? error.message : String(error)
    } finally {
      busy = false
    }
  }

  function positionAtViewportCenter(): XYPosition {
    if (canvas === undefined) {
      return { x: 80 + (nodes.length % 4) * 290, y: 80 + Math.floor(nodes.length / 4) * 230 }
    }
    const bounds = canvas.getBoundingClientRect()
    return screenToFlowPosition(
      { x: bounds.left + bounds.width / 2, y: bounds.top + bounds.height / 2 },
      { snapToGrid: snapEnabled },
    )
  }

  function handleDragOver(event: DragEvent): void {
    event.preventDefault()
    if (event.dataTransfer !== null) event.dataTransfer.dropEffect = 'move'
  }

  function handleDrop(event: DragEvent): void {
    event.preventDefault()
    const nodeType = event.dataTransfer?.getData(NODE_DRAG_MIME)
    if (nodeType === undefined || nodeType === '' || busy) return
    const position = screenToFlowPosition(
      { x: event.clientX, y: event.clientY },
      { snapToGrid: snapEnabled },
    )
    void addNode(nodeType, position)
  }

  async function connect(connection: Connection): Promise<void> {
    if (connection.sourceHandle === null || connection.targetHandle === null) {
      await reloadState()
      return
    }
    if (!nativeConnected) {
      edges = [
        ...edges,
        {
          id: [connection.source, connection.sourceHandle, connection.target, connection.targetHandle]
            .map(encodeURIComponent)
            .join('--'),
          source: connection.source,
          sourceHandle: connection.sourceHandle,
          target: connection.target,
          targetHandle: connection.targetHandle,
          type: 'smoothstep',
        },
      ]
      message = 'Connected browser-only diagnostic nodes'
      return
    }
    busy = true
    failed = false
    try {
      await callTapioca('Tapioca.GraphApplyEdit', {
        editKind: 'connect',
        sourceNode: connection.source,
        sourcePort: connection.sourceHandle,
        targetNode: connection.target,
        targetPort: connection.targetHandle,
      })
      message = `Connected ${connection.source} to ${connection.target}`
    } catch (error) {
      failed = true
      message = error instanceof Error ? error.message : String(error)
    } finally {
      await reloadState()
      busy = false
    }
  }

  const isValidConnection: IsValidConnection = (connection) =>
    isCatalogConnectionValid(connection, nodes, edges)

  function requestConnection(connection: Connection): false {
    if (!isCatalogConnectionValid(connection, nodes, edges)) {
      failed = true
      message = 'Connection rejected by the native catalog projection.'
      return false
    }
    void connect(connection)
    return false
  }

  async function removeElements(selectedNodes: Node<SchemaNodeData>[], selectedEdges: Edge[]): Promise<void> {
    if (selectedNodes.length === 0 && selectedEdges.length === 0) return
    if (!nativeConnected) {
      const removedNodeIds = new Set(selectedNodes.map((node) => node.id))
      const removedEdgeIds = new Set(selectedEdges.map((edge) => edge.id))
      nodes = nodes.filter((node) => !removedNodeIds.has(node.id))
      edges = edges.filter(
        (edge) =>
          !removedEdgeIds.has(edge.id) && !removedNodeIds.has(edge.source) && !removedNodeIds.has(edge.target),
      )
      message = `Removed ${selectedNodes.length} browser-only nodes and ${selectedEdges.length} connections`
      return
    }
    busy = true
    failed = false
    try {
      for (const edge of selectedEdges) {
        await callTapioca('Tapioca.GraphApplyEdit', {
          editKind: 'disconnect',
          sourceNode: edge.source,
          sourcePort: edge.sourceHandle,
          targetNode: edge.target,
          targetPort: edge.targetHandle,
        })
      }
      for (const node of selectedNodes) {
        await callTapioca('Tapioca.GraphApplyEdit', { editKind: 'removeNode', nodeId: node.id })
        positions.delete(node.id)
      }
      message = `Removed ${selectedNodes.length} nodes and ${selectedEdges.length} connections`
    } catch (error) {
      failed = true
      message = error instanceof Error ? error.message : String(error)
    } finally {
      await reloadState()
      busy = false
    }
  }

  async function removeSelection(): Promise<void> {
    await removeElements(
      nodes.filter((node) => node.selected),
      edges.filter((edge) => edge.selected),
    )
  }

  async function removeFromRuntime({
    nodes: selectedNodes,
    edges: selectedEdges,
  }: {
    nodes: Node<SchemaNodeData>[]
    edges: Edge[]
  }): Promise<boolean> {
    if (!busy) await removeElements(selectedNodes, selectedEdges)
    return false
  }

  function handleKeyDown(event: KeyboardEvent): void {
    if (event.key !== 'Escape') return
    if (contextTarget !== null) {
      contextTarget = null
      event.preventDefault()
      return
    }
    const target = event.target
    if (
      target instanceof HTMLElement &&
      (target.isContentEditable || ['INPUT', 'SELECT', 'TEXTAREA'].includes(target.tagName))
    ) {
      target.blur()
    }

    nodes = nodes.map((node) => (node.selected ? { ...node, selected: false } : node))
    edges = edges.map((edge) => (edge.selected ? { ...edge, selected: false } : edge))
    event.preventDefault()
  }

  async function evaluate(): Promise<void> {
    if (!nativeConnected) {
      message = 'Evaluation requires the native graph runtime; interaction timing does not.'
      return
    }
    busy = true
    failed = false
    try {
      await callTapioca('Tapioca.GraphEvaluate')
      await refreshResults()
      await reloadState()
      const complete = results.filter((result) => result.status === 'complete').length
      message = `Evaluation complete: ${complete}/${results.length} nodes / revision ${revision}`
    } catch (error) {
      failed = true
      message = error instanceof Error ? error.message : String(error)
      await refreshResults()
      await reloadState()
    } finally {
      busy = false
    }
  }

  function rememberPosition({ targetNode }: { targetNode: Node<SchemaNodeData> | null }): void {
    if (targetNode !== null) positions.set(targetNode.id, { ...targetNode.position })
  }

  function setTheme(value: ThemeMode): void {
    theme = value
    localStorage.setItem('tapioca.graph.theme', value)
  }

  function toggleSnap(): void {
    snapEnabled = !snapEnabled
    message = `Grid snap ${snapEnabled ? 'enabled at 16 px' : 'disabled'}`
  }

  function handleMove(_event: MouseEvent | TouchEvent | null, viewport: { zoom: number }): void {
    detailLevel = detailLevelForZoom(viewport.zoom, detailLevel)
  }

  function contextPosition(event: MouseEvent): { x: number; y: number } {
    const bounds = canvas?.getBoundingClientRect()
    return {
      x: Math.max(8, Math.min(event.clientX - (bounds?.left ?? 0), (bounds?.width ?? 240) - 200)),
      y: Math.max(8, Math.min(event.clientY - (bounds?.top ?? 0), (bounds?.height ?? 180) - 120)),
    }
  }

  function openPaneContext({ event }: { event: MouseEvent }): void {
    event.preventDefault()
    contextTarget = { kind: 'pane', ...contextPosition(event) }
  }

  function openNodeContext({ node, event }: { node: Node<SchemaNodeData>; event: MouseEvent }): void {
    event.preventDefault()
    contextTarget = { kind: 'node', node, ...contextPosition(event) }
  }

  function openEdgeContext({ edge, event }: { edge: Edge; event: MouseEvent }): void {
    event.preventDefault()
    contextTarget = { kind: 'edge', edge, ...contextPosition(event) }
  }

  function contextLabel(): string {
    if (contextTarget?.kind === 'node') return contextTarget.node.data.schema.label
    if (contextTarget?.kind === 'edge') return 'Connection'
    return 'Canvas'
  }

  function contextActions() {
    if (contextTarget?.kind === 'node') {
      const node = contextTarget.node
      return [
        { label: 'Fit node', run: () => void fitView({ nodes: [node], duration: 180, padding: 0.8 }) },
        { label: 'Delete node', disabled: busy, run: () => void removeElements([node], []) },
      ]
    }
    if (contextTarget?.kind === 'edge') {
      const edge = contextTarget.edge
      return [{ label: 'Disconnect', disabled: busy, run: () => void removeElements([], [edge]) }]
    }
    return [
      { label: 'Open components', run: () => (pickerOpen = true) },
      { label: 'Fit graph', disabled: nodes.length === 0, run: () => void fitView({ duration: 180, padding: 0.2 }) },
    ]
  }

  function movePlainMarker(event: PointerEvent): void {
    if (diagnosticMode !== 'plain-marker') return
    moveMarker(event)
  }

  function moveRawMarker(event: PointerEvent): void {
    if (diagnosticMode !== 'raw-marker') return
    moveMarker(event)
  }

  function moveMarker(event: PointerEvent): void {
    if (marker === undefined || canvas === undefined) return
    const bounds = canvas.getBoundingClientRect()
    marker.style.transform = `translate3d(${event.clientX - bounds.left - 12}px, ${event.clientY - bounds.top - 12}px, 0)`
  }

  onMount(() => {
    theme = initialTheme(localStorage)
    const rawPointer = (event: Event) => moveRawMarker(event as PointerEvent)
    window.addEventListener('pointerrawupdate', rawPointer, { passive: true })
    void initialize()
    return () => window.removeEventListener('pointerrawupdate', rawPointer)
  })
</script>

  <svelte:window onkeydown={handleKeyDown} />

<main data-theme={theme}>
  <header class="toolbar">
    <div class="brand">
      <span>Tapioca / Experimental</span>
      <strong>Node Graph</strong>
    </div>
    <ApplicationMenu
      {busy}
      {nativeConnected}
      {snapEnabled}
      {theme}
      {performanceOpen}
      onrefresh={() => void reloadState()}
      onevaluate={() => void evaluate()}
      onopenpicker={() => (pickerOpen = true)}
      ontogglesnap={toggleSnap}
      ontheme={setTheme}
      ontoggleperformance={() => (performanceOpen = !performanceOpen)}
    />
    <div class="actions">
      <button onclick={removeSelection} disabled={busy}>Erase selected</button>
    </div>
  </header>

  <section
    class="canvas"
    class:picker-open={pickerOpen}
    aria-label="Node graph canvas"
    data-detail-level={detailLevel}
    onpointermove={movePlainMarker}
    bind:this={canvas}
  >
    <ComponentPicker
      {catalog}
      {busy}
      open={pickerOpen}
      onopen={() => (pickerOpen = true)}
      onclose={() => (pickerOpen = false)}
      onplace={(nodeType) => void addNode(nodeType)}
    />
    {#if diagnosticMode !== 'plain-marker' && diagnosticMode !== 'raw-marker'}
      <SvelteFlow
      bind:nodes
      bind:edges
      {nodeTypes}
      fitView
      minZoom={0.15}
      maxZoom={2.5}
      colorMode={theme as ColorMode}
      colorModeSSR="dark"
      snapGrid={snapEnabled ? SNAP_GRID : undefined}
      {isValidConnection}
      onbeforeconnect={requestConnection}
      onbeforedelete={removeFromRuntime}
      onnodedragstop={rememberPosition}
      onmove={handleMove}
      ondragover={handleDragOver}
      ondrop={handleDrop}
      onpaneclick={() => (contextTarget = null)}
      onpanecontextmenu={openPaneContext}
      onnodecontextmenu={openNodeContext}
      onedgecontextmenu={openEdgeContext}
      deleteKey={['Backspace', 'Delete']}
      >
        {#if diagnosticMode === 'flow'}
          <Background variant={BackgroundVariant.Dots} gap={20} size={1} />
          <Controls position="bottom-left" />
          <MiniMap position="bottom-right" pannable zoomable />
        {/if}
      </SvelteFlow>
    {:else}
      <div class="plain-stage">
        <div class="plain-marker" bind:this={marker}></div>
        <p>Move continuously. This div follows {diagnosticMode === 'raw-marker' ? 'pointerrawupdate' : 'pointermove'} through one direct CSS transform with no Svelte state update.</p>
      </div>
    {/if}

    {#if contextTarget !== null}
      <ContextMenu
        x={contextTarget.x}
        y={contextTarget.y}
        label={contextLabel()}
        actions={contextActions()}
        onclose={() => (contextTarget = null)}
      />
    {/if}

    {#if nodes.length === 0 && !busy && !failed}
      <div class="empty">
        <span>Revision {revision}</span>
        <h1>The runtime is empty.</h1>
        <p>Add a Number node, then connect native ports. Every accepted edit is owned by C++.</p>
      </div>
    {/if}

    {#if performanceOpen}
      <PerformancePanel
        bind:mode={diagnosticMode}
        nodeCount={nodes.length}
        edgeCount={edges.length}
        onclose={() => (performanceOpen = false)}
      />
    {/if}
  </section>

  <footer class:error={failed}>
    <span class:active={busy}></span>
    <p>{message}</p>
    <code>{nativeConnected ? `rev ${revision}` : 'fixture'}</code>
  </footer>
</main>
