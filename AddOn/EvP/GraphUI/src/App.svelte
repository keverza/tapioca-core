<script lang="ts">
  import {
    Background,
    BackgroundVariant,
    Controls,
    MiniMap,
    SvelteFlow,
    type Connection,
    type Edge,
    type Node,
    type NodeTypes,
  } from '@xyflow/svelte'
  import '@xyflow/svelte/dist/style.css'
  import { onMount } from 'svelte'
  import { callTapioca, isNativeBridgeAvailable } from './bridge'
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

  let nodes = $state.raw<Node<SchemaNodeData>[]>([])
  let edges = $state.raw<Edge[]>([])
  let catalog = $state.raw<NodeTypeSchema[]>([])
  let results = $state.raw<NodeResultRecord[]>([])
  let selectedNodeType = $state('')
  let revision = $state(0)
  let busy = $state(false)
  let message = $state('Connecting to the native graph runtime...')
  let failed = $state(false)
  let performanceOpen = $state(false)
  let diagnosticMode = $state<DiagnosticMode>('flow')
  let snapEnabled = $state(true)
  let nativeConnected = $state(isNativeBridgeAvailable())
  let marker = $state<HTMLDivElement>()

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
      selectedNodeType = catalog[0]?.nodeType ?? ''
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
    selectedNodeType = catalog[0].nodeType
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

  async function addNode(): Promise<void> {
    if (selectedNodeType === '') return
    if (!nativeConnected) {
      const schema = catalog.find((item) => item.nodeType === selectedNodeType)
      if (schema === undefined) return
      const nodeId = `${selectedNodeType}-${Date.now().toString(36)}`
      nodes = [
        ...nodes,
        {
          id: nodeId,
          type: 'schema',
          position: positionFor(nodeId, nodes.length),
          data: { schema, parameters: [] },
        },
      ]
      message = `Added browser-only ${nodeId}`
      return
    }
    busy = true
    failed = false
    try {
      const nodeId = `${selectedNodeType}-${Date.now().toString(36)}`
      const params: Record<string, unknown> = {
        editKind: 'addNode',
        nodeId,
        nodeType: selectedNodeType,
      }
      if (selectedNodeType === 'number') params.numberValue = 0
      await callTapioca('Tapioca.GraphApplyEdit', params)
      await reloadState()
      message = `Added ${nodeId} / revision ${revision}`
    } catch (error) {
      failed = true
      message = error instanceof Error ? error.message : String(error)
    } finally {
      busy = false
    }
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

  function movePlainMarker(event: PointerEvent): void {
    if (diagnosticMode !== 'plain-marker' || marker === undefined) return
    const bounds = (event.currentTarget as HTMLElement).getBoundingClientRect()
    marker.style.transform = `translate3d(${event.clientX - bounds.left - 12}px, ${event.clientY - bounds.top - 12}px, 0)`
  }

  function closeMenu(event: MouseEvent): void {
    const menu = (event.currentTarget as HTMLElement).closest('details')
    menu?.removeAttribute('open')
  }

  function toggleSnap(event: MouseEvent): void {
    snapEnabled = !snapEnabled
    message = `Grid snap ${snapEnabled ? 'enabled at 16 px' : 'disabled'}`
    closeMenu(event)
  }

  onMount(() => {
    void initialize()
  })
</script>

<svelte:window onkeydown={handleKeyDown} />

<main>
  <header class="toolbar">
    <div class="brand">
      <span>Tapioca / Experimental</span>
      <strong>Node Graph</strong>
    </div>
    <nav class="menu-bar" aria-label="Application menu">
      <details>
        <summary>File</summary>
        <div class="menu-popover">
          <button
            onclick={(event) => {
              closeMenu(event)
              void reloadState()
            }}
            disabled={busy || !nativeConnected}>Refresh graph</button
          >
        </div>
      </details>
      <details>
        <summary>Run</summary>
        <div class="menu-popover">
          <button
            onclick={(event) => {
              closeMenu(event)
              void evaluate()
            }}
            disabled={busy}>Evaluate graph</button
          >
        </div>
      </details>
      <details>
        <summary>Flow</summary>
        <div class="menu-popover">
          <button class="menu-toggle" role="menuitemcheckbox" aria-checked={snapEnabled} onclick={toggleSnap}>
            <span class:checked={snapEnabled} aria-hidden="true"></span>
            Snap to grid
            <kbd>16 px</kbd>
          </button>
        </div>
      </details>
      <details>
        <summary>Debug</summary>
        <div class="menu-popover menu-right">
          <button
            class="menu-toggle"
            role="menuitemcheckbox"
            aria-checked={performanceOpen}
            onclick={(event) => {
              performanceOpen = !performanceOpen
              closeMenu(event)
            }}
          >
            <span class:checked={performanceOpen} aria-hidden="true"></span>
            Interaction timing
          </button>
        </div>
      </details>
    </nav>
    <div class="actions">
      <select bind:value={selectedNodeType} disabled={busy || catalog.length === 0} aria-label="Node type">
        {#each catalog as item}
          <option value={item.nodeType}>{item.category} / {item.label}</option>
        {/each}
      </select>
      <button class="primary" onclick={addNode} disabled={busy || selectedNodeType === ''}>Add node</button>
      <button onclick={removeSelection} disabled={busy}>Remove selected</button>
    </div>
  </header>

  <section class="canvas" aria-label="Node graph canvas" onpointermove={movePlainMarker}>
    {#if diagnosticMode !== 'plain-marker'}
      <SvelteFlow
      bind:nodes
      bind:edges
      {nodeTypes}
      fitView
      minZoom={0.15}
      maxZoom={2.5}
      snapGrid={snapEnabled ? SNAP_GRID : undefined}
      onconnect={connect}
      onbeforeconnect={(connection) =>
        connection.sourceHandle !== null && connection.targetHandle !== null ? connection : false}
      onbeforedelete={removeFromRuntime}
      onnodedragstop={rememberPosition}
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
        <p>Move the pointer continuously. This div follows through one direct CSS transform with no Svelte state update.</p>
      </div>
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
