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
  import { callTapioca } from './bridge'
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

  let nodes = $state.raw<Node<SchemaNodeData>[]>([])
  let edges = $state.raw<Edge[]>([])
  let catalog = $state.raw<NodeTypeSchema[]>([])
  let results = $state.raw<NodeResultRecord[]>([])
  let selectedNodeType = $state('')
  let revision = $state(0)
  let busy = $state(false)
  let message = $state('Connecting to the native graph runtime...')
  let failed = $state(false)

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

  async function reloadState(): Promise<void> {
    applyState(await callTapioca<GraphState>('Tapioca.GraphGetState'))
  }

  async function initialize(): Promise<void> {
    busy = true
    failed = false
    try {
      const response = await callTapioca<{ nodeTypes: NodeTypeSchema[] }>('Tapioca.GraphGetNodeTypes')
      catalog = response.nodeTypes
      selectedNodeType = catalog[0]?.nodeType ?? ''
      await reloadState()
      message = `${catalog.length} native node types / revision ${revision}`
    } catch (error) {
      failed = true
      message = error instanceof Error ? error.message : String(error)
    } finally {
      busy = false
    }
  }

  async function addNode(): Promise<void> {
    if (selectedNodeType === '') return
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
    busy = true
    failed = false
    try {
      await callTapioca('Tapioca.GraphEvaluate')
      const response = await callTapioca<{ revision: number; results: NodeResultRecord[] }>(
        'Tapioca.GraphGetNodeResults',
      )
      results = response.results
      await reloadState()
      const complete = results.filter((result) => result.status === 'complete').length
      message = `Evaluation complete: ${complete}/${results.length} nodes / revision ${revision}`
    } catch (error) {
      failed = true
      message = error instanceof Error ? error.message : String(error)
      const response = await callTapioca<{ results: NodeResultRecord[] }>('Tapioca.GraphGetNodeResults')
      results = response.results
      await reloadState()
    } finally {
      busy = false
    }
  }

  function rememberPosition({ targetNode }: { targetNode: Node<SchemaNodeData> | null }): void {
    if (targetNode !== null) positions.set(targetNode.id, { ...targetNode.position })
  }

  $effect(() => {
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
    <div class="actions">
      <select bind:value={selectedNodeType} disabled={busy || catalog.length === 0} aria-label="Node type">
        {#each catalog as item}
          <option value={item.nodeType}>{item.category} / {item.label}</option>
        {/each}
      </select>
      <button class="primary" onclick={addNode} disabled={busy || selectedNodeType === ''}>Add node</button>
      <button onclick={removeSelection} disabled={busy}>Remove selected</button>
      <button onclick={reloadState} disabled={busy}>Refresh</button>
      <button class="run" onclick={evaluate} disabled={busy}>Evaluate</button>
    </div>
  </header>

  <section class="canvas" aria-label="Node graph canvas">
    <SvelteFlow
      bind:nodes
      bind:edges
      {nodeTypes}
      fitView
      minZoom={0.15}
      maxZoom={2.5}
      snapGrid={[16, 16]}
      onconnect={connect}
      onbeforeconnect={(connection) =>
        connection.sourceHandle !== null && connection.targetHandle !== null ? connection : false}
      onbeforedelete={removeFromRuntime}
      onnodedragstop={rememberPosition}
      deleteKey={['Backspace', 'Delete']}
    >
      <Background variant={BackgroundVariant.Dots} gap={20} size={1} />
      <Controls position="bottom-left" />
      <MiniMap position="bottom-right" pannable zoomable />
    </SvelteFlow>

    {#if nodes.length === 0 && !busy && !failed}
      <div class="empty">
        <span>Revision {revision}</span>
        <h1>The runtime is empty.</h1>
        <p>Add a Number node, then connect native ports. Every accepted edit is owned by C++.</p>
      </div>
    {/if}
  </section>

  <footer class:error={failed}>
    <span class:active={busy}></span>
    <p>{message}</p>
    <code>rev {revision}</code>
  </footer>
</main>
