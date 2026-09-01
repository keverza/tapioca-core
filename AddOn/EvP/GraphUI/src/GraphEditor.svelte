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
  import AnnotationLayer from './AnnotationLayer.svelte'
  import {
    annotationAtPoint,
    annotationFromSelection,
    boundsFromPoints,
    loadAnnotations,
    removeAnnotation,
    renameAnnotation,
    resolveFrameBounds,
    saveAnnotations,
    type EditorAnnotation,
    type EditorTool,
    type EffectiveTool,
  } from './annotations'
  import ApplicationMenu from './ApplicationMenu.svelte'
  import { callTapioca, isNativeBridgeAvailable, waitForNativeBridge } from './bridge'
  import ComponentPicker from './ComponentPicker.svelte'
  import ContextMenu from './ContextMenu.svelte'
  import { parsePortReference, serializePortReference } from './nodes/types/portReference'
  import {
    applyLayoutToPositions,
    describeValueRule,
    detailLevelForZoom,
    graphValueFromText,
    initialTheme,
    isCatalogConnectionValid,
    NODE_DRAG_MIME,
    type DetailLevel,
    type ThemeMode,
  } from './editor'
  import LibraryDialog, { type LibraryMode } from './LibraryDialog.svelte'
  import {
    applySelectionAction,
    browseForGraph,
    deleteGraph,
    listGraphs,
    loadGraph,
    saveGraph,
    type StoredGraphInfo,
  } from './library'
  import NodeSearch from './NodeSearch.svelte'
  import PerformancePanel from './PerformancePanel.svelte'
  import type { DiagnosticMode } from './performance'
  import MasterNode from './nodes/MasterNode.svelte'
  import { requestQuickMenu, requestRename } from './nodes/renameRequest.svelte'
  import { parseNodePresentations, serializeNodePresentations } from './nodes/serialization'
  import ToolStrip from './ToolStrip.svelte'
  import type {
    EvaluationSummary,
    ExecutionMode,
    GraphEdgeRecord,
    GraphState,
    NodeResultRecord,
    NodeTypeSchema,
    PortReference,
    PositionStore,
    SchemaNodeData,
    SelectionAction,
    NodeVisualState,
  } from './types'

  const nodeTypes: NodeTypes = { schema: MasterNode }
  /**
   * MIDDLE mouse pans; left drag is a selection rectangle.
   *
   * Panning on left-drag is what every node editor this one is measured against
   * does NOT do, and it cost the rubber-band selection to have it: a drag on
   * empty canvas is how a person selects a group, and the pan is a deliberate
   * middle-button gesture. 1 is the middle button.
   */
  const PAN_BUTTONS = [1]
  const positions: PositionStore = new Map()
  const visuals = new Map<string, NodeVisualState>()
  const SNAP_GRID: [number, number] = [16, 16]
  const { fitView, screenToFlowPosition } = useSvelteFlow<Node<SchemaNodeData>, Edge>()

  type ContextTarget =
    | { kind: 'pane'; x: number; y: number }
    | { kind: 'node'; x: number; y: number; node: Node<SchemaNodeData> }
    | { kind: 'edge'; x: number; y: number; edge: Edge }
    | { kind: 'port'; x: number; y: number; nodeId: string; portId: string; direction: 'input' | 'output' }

  type ToolGesture =
    | { kind: 'rectangle'; pointerId: number; start: XYPosition }
    | { kind: 'erase'; pointerId: number; last: XYPosition }

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
  let activeTool = $state<EditorTool>('select')
  let controlHeld = $state(false)
  let controlChord = $state(false)
  let toolGesture = $state<ToolGesture | null>(null)
  let erasedNodeIds = $state.raw<string[]>([])
  let erasedEdgeIds = $state.raw<string[]>([])
  let annotations = $state.raw<EditorAnnotation[]>([])
  let annotationDraft = $state<EditorAnnotation>()
  // Presentation-only rectangles are selected here rather than by the flow
  // library, which knows nothing about them: they are drawn on the layer BEHIND
  // the nodes and take no pointer events, so the canvas hit-tests their stored
  // bounds instead. See annotationAtPoint.
  let selectedAnnotationId = $state<string | null>(null)
  // Double-click-anywhere component search. Carries the flow-space position the
  // node lands on as well as the screen point the palette is drawn at.
  let searchTarget = $state<{ x: number; y: number; position: XYPosition } | null>(null)
  /**
   * A locked solution refuses to evaluate. Editing stays open - the point is to
   * change several things without paying for a run after each one - and the
   * runtime is never told, because the runtime does not evaluate unless it is
   * asked. Locking is therefore exactly "stop asking".
   */
  let solutionLocked = $state(false)
  let graphId = $state('default')
  let nativeConnected = $state(isNativeBridgeAvailable())
  let marker = $state<HTMLDivElement>()
  let canvas = $state<HTMLElement>()

  // The workflow library. `libraryMode` null means the dialog is closed.
  let libraryMode = $state<LibraryMode | null>(null)
  let libraryGraphs = $state.raw<StoredGraphInfo[]>([])
  let libraryLocation = $state('')
  let libraryError = $state('')
  let libraryBusy = $state(false)
  let currentGraphName = $state('')

  // The selection-set node whose buttons are mid-action, so only that node's
  // five buttons disable rather than the whole canvas going busy for what is a
  // sub-second round trip.
  let selectionBusyNode = $state<string | null>(null)

  const effectiveTool = $derived<EffectiveTool>(controlHeld && !controlChord ? 'knife' : activeTool)
  const resolvedAnnotations = $derived(
    annotations.map((annotation) => resolveFrameBounds(annotation, nodes)),
  )

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
    graphId = state.graphId ?? 'default'
    const schemas = new Map(catalog.map((item) => [item.nodeType, item]))
    const resultMap = new Map(results.map((item) => [item.nodeId, item]))
    nodes = state.nodes.map((node, index) => ({
      id: node.nodeId,
      type: 'schema',
      position: positionFor(node.nodeId, index),
      data: {
        onvisualchange: handleVisualChange,
        visual: visuals.get(node.nodeId),
        onexecutionchange: (nodeId, mode) => void setExecutionMode(nodeId, mode),
        onparameterchange: (nodeId, parameterId, valueType, text) =>
          void setParameter(nodeId, parameterId, valueType, text),
        onportreference: (reference, target) => connectReference(reference, target),
        onportcontextmenu: (event, target) => openPortContext(event, target),
        portConnections: [
          ...(schemas.get(node.nodeType)?.inputs ?? []).map((port) => {
            const matching = state.edges.filter((edge) => edge.targetNode === node.nodeId && edge.targetPort === port.portId)
            return { portId: port.portId, direction: 'input' as const, connected: matching.length > 0, connectionCount: matching.length, peerLabels: matching.map((edge) => `${edge.sourceNode}.${edge.sourcePort}`) }
          }),
          ...(schemas.get(node.nodeType)?.outputs ?? []).map((port) => {
            const matching = state.edges.filter((edge) => edge.sourceNode === node.nodeId && edge.sourcePort === port.portId)
            return { portId: port.portId, direction: 'output' as const, connected: matching.length > 0, connectionCount: matching.length, peerLabels: matching.map((edge) => `${edge.targetNode}.${edge.targetPort}`) }
          }),
        ],
        onselectionaction: handleSelectionAction,
        selectionBusy: selectionBusyNode === node.nodeId,
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
        executionMode: node.executionMode ?? 'enabled',
        result: resultMap.get(node.nodeId),
        messages: (() => {
          const result = resultMap.get(node.nodeId)
          if (result === undefined || result.message === '') return []
          return [{ severity: result.status === 'error' ? 'error' as const : result.status === 'blocked' ? 'warning' as const : 'info' as const, code: result.code ?? result.status, title: result.message, nodeId: node.nodeId }]
        })(),
      },
      style: `width: ${schemas.get(node.nodeType)?.display === 'preview' ? 292 : 248}px`,
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

  function visualStorageKey(): string {
    return `tapioca.graph.visuals.${currentGraphName || graphId}`
  }

  function loadVisuals(): void {
    visuals.clear()
    try {
      const stored = JSON.parse(localStorage.getItem(visualStorageKey()) ?? '{"version":1,"nodes":[]}')
      for (const [nodeId, visual] of parseNodePresentations(stored)) visuals.set(nodeId, visual)
    } catch {
      // Invalid presentation metadata must not prevent the native graph from opening.
    }
  }

  function persistVisuals(): void {
    localStorage.setItem(visualStorageKey(), JSON.stringify(serializeNodePresentations(visuals)))
  }

  function handleVisualChange(nodeId: string, visual: NodeVisualState): void {
    if (visual.nickname === undefined && visual.color === undefined) visuals.delete(nodeId)
    else visuals.set(nodeId, visual)
    persistVisuals()
    nodes = nodes.map((node) => node.id === nodeId ? { ...node, data: { ...node.data, visual } } : node)
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
      // The bridge may still be arriving; see waitForNativeBridge. Deciding
      // "there is no runtime" on the first synchronous sample is what makes the
      // component picker show two diagnostic nodes instead of the catalog.
      nativeConnected = await waitForNativeBridge()
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
      const state = await callTapioca<GraphState>('Tapioca.GraphGetState')
      graphId = state.graphId ?? 'default'
      loadVisuals()
      applyState(state)
      loadEditorAnnotations()
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
    loadVisuals()
    applyState({
      revision: 0,
      nodes: [
        { nodeId: 'diagnostic-source', nodeType: 'diagnostic.source', parameters: [] },
        { nodeId: 'diagnostic-sink', nodeType: 'diagnostic.sink', parameters: [] },
      ],
      edges: [{ sourceNode: 'diagnostic-source', sourcePort: 'value', targetNode: 'diagnostic-sink', targetPort: 'value' }],
    })
    loadEditorAnnotations()
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

  /**
   * A value typed into a control.
   *
   * Goes through Tapioca.GraphApplyEdit like a wire does, because that is what
   * it is: a document edit the runtime owns. The text is turned into the
   * runtime's encoding HERE and refused here when it is not one - a box that
   * will not take "six" says so immediately, where a round trip would answer
   * with a type-mismatch two states later.
   */
  async function setParameter(nodeId: string, parameterId: string, valueType: string, text: string): Promise<void> {
    const value = graphValueFromText(valueType, text)
    if (value === undefined) {
      if (text.trim() === '') return
      failed = true
      message = `"${text.trim()}" is not ${describeValueRule(valueType)}.`
      return
    }
    if (!nativeConnected) {
      message = 'Typed-in values need the native graph runtime.'
      return
    }
    busy = true
    failed = false
    try {
      await callTapioca('Tapioca.GraphApplyEdit', { graphId, editKind: 'setParam', nodeId, parameterId, value })
      await reloadState()
      message = `${nodeId}.${parameterId} = ${text.trim()} / revision ${revision}`
    } catch (error) {
      failed = true
      message = error instanceof Error ? error.message : String(error)
      await reloadState()
    } finally {
      busy = false
    }
  }

  /**
   * A port reference pasted onto an input.
   *
   * The clipboard names a port; the only thing that can be done with a named
   * port is wire it up, so this is a connection request and goes through the
   * same validation every dragged wire does.
   */
  function connectReference(reference: PortReference, target: { nodeId: string; portId: string }): void {
    if (reference.direction !== 'output') {
      failed = true
      message = 'That reference names an input. Copy an OUTPUT port to paste it onto an input.'
      return
    }
    if (!nodes.some((node) => node.id === reference.nodeId)) {
      failed = true
      message = `The reference names ${reference.nodeId}, which is not in this graph.`
      return
    }
    requestConnection({
      source: reference.nodeId,
      sourceHandle: reference.portId,
      target: target.nodeId,
      targetHandle: target.portId,
    })
  }

  function copyPortReference(nodeId: string, portId: string, direction: 'input' | 'output'): void {
    const node = nodes.find((item) => item.id === nodeId)
    const ports = direction === 'input' ? node?.data.schema.inputs : node?.data.schema.outputs
    const valueType = ports?.find((port) => port.portId === portId)?.valueType ?? ''
    void navigator.clipboard?.writeText(
      serializePortReference({ kind: 'nodePort', nodeId, portId, direction, valueType }),
    )
    message = `Copied a reference to ${nodeId}.${portId}`
    failed = false
  }

  /** Paste onto an input: the clipboard names a port, so this wires it up. */
  async function pastePortReference(nodeId: string, portId: string): Promise<void> {
    let text: string | undefined
    try {
      text = await navigator.clipboard?.readText()
    } catch {
      failed = true
      message = 'The clipboard is not readable here. Copy a port reference and paste it into the input box instead.'
      return
    }
    const reference = text === undefined ? undefined : parsePortReference(text)
    if (reference === undefined) {
      failed = true
      message = 'The clipboard does not hold a port reference.'
      return
    }
    connectReference(reference, { nodeId, portId })
  }

  /** Every wire on one port, dropped in a single erase. */
  async function disconnectPort(nodeId: string, portId: string, direction: 'input' | 'output'): Promise<void> {
    const affected = edges.filter((edge) =>
      direction === 'input'
        ? edge.target === nodeId && edge.targetHandle === portId
        : edge.source === nodeId && edge.sourceHandle === portId,
    )
    if (affected.length === 0) return
    await removeElements([], affected)
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
      removeNodesFromFrames(removedNodeIds)
      for (const nodeId of removedNodeIds) visuals.delete(nodeId)
      persistVisuals()
      message = `Removed ${selectedNodes.length} browser-only nodes and ${selectedEdges.length} connections`
      return
    }
    busy = true
    failed = false
    try {
      await callTapioca('Tapioca.GraphEraseElements', {
        nodeIds: selectedNodes.map((node) => node.id),
        edges: selectedEdges.map((edge) => ({
          sourceNode: edge.source,
          sourcePort: edge.sourceHandle ?? '',
          targetNode: edge.target,
          targetPort: edge.targetHandle ?? '',
        })),
      })
      for (const node of selectedNodes)
        positions.delete(node.id)
      removeNodesFromFrames(new Set(selectedNodes.map((node) => node.id)))
      for (const node of selectedNodes) visuals.delete(node.id)
      persistVisuals()
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
    const target = event.target
    const editing =
      target instanceof HTMLElement &&
      (target.isContentEditable || ['INPUT', 'SELECT', 'TEXTAREA'].includes(target.tagName))

    if (event.key === 'Control') {
      if (!editing) controlHeld = true
      return
    }
    if (event.ctrlKey && event.key.toLocaleLowerCase() === 'g' && !editing) {
      controlChord = true
      createFrameFromSelection()
      event.preventDefault()
      return
    }
    // Delete removes the selected annotation. The flow library's own delete key
    // only reaches nodes and edges, and an annotation is neither.
    if (
      (event.key === 'Delete' || event.key === 'Backspace') &&
      !editing &&
      selectedAnnotationId !== null &&
      !nodes.some((node) => node.selected) &&
      !edges.some((edge) => edge.selected)
    ) {
      deleteAnnotation(selectedAnnotationId)
      event.preventDefault()
      return
    }
    if (event.key !== 'Escape') return
    if (searchTarget !== null) {
      searchTarget = null
      event.preventDefault()
      return
    }
    if (contextTarget !== null) {
      contextTarget = null
      event.preventDefault()
      return
    }
    if (editing && target instanceof HTMLElement) {
      target.blur()
    }

    activeTool = 'select'
    toolGesture = null
    annotationDraft = undefined
    selectedAnnotationId = null
    clearErasePreview()

    nodes = nodes.map((node) => (node.selected ? { ...node, selected: false } : node))
    edges = edges.map((edge) => (edge.selected ? { ...edge, selected: false } : edge))
    event.preventDefault()
  }

  function handleKeyUp(event: KeyboardEvent): void {
    if (event.key !== 'Control') return
    controlHeld = false
    controlChord = false
  }

  // ADR-007's gate is an A/B on one graph rather than a claim, so the sequential
  // arm is a menu item next to the ordinary one and the numbers land in the
  // status line where both runs can be compared by eye.
  function describeParallelism(summary: EvaluationSummary): string {
    const p = summary.parallelism
    if (p === undefined) return 'no parallelism reported'
    return (
      `peak ${p.peakConcurrency}x of ${p.maxParallel} / speedup ${p.speedup.toFixed(2)} / ` +
      `${p.wallClockMs.toFixed(1)} ms wall of ${p.workMs.toFixed(1)} ms work`
    )
  }

  async function evaluate(maxParallel = 0): Promise<void> {
    if (solutionLocked) {
      failed = true
      message = 'The solution is locked. Resume it to evaluate.'
      return
    }
    if (!nativeConnected) {
      message = 'Evaluation requires the native graph runtime; interaction timing does not.'
      return
    }
    busy = true
    failed = false
    try {
      const summary = await callTapioca<EvaluationSummary>('Tapioca.GraphEvaluate', { maxParallel })
      await refreshResults()
      await reloadState()
      const complete = results.filter((result) => result.status === 'success').length
      message = `Evaluation complete: ${complete}/${results.length} nodes / revision ${revision} / ${describeParallelism(summary)}`
    } catch (error) {
      failed = true
      message = error instanceof Error ? error.message : String(error)
      await refreshResults()
      await reloadState()
    } finally {
      busy = false
    }
  }

  /**
   * One of the selection set's five buttons.
   *
   * The runtime changes the set AND evaluates what the change reaches, so this
   * only has to redraw. That is the whole point of routing it through a verb
   * rather than an edit followed by a manual Evaluate.
   */
  async function handleSelectionAction(nodeId: string, action: SelectionAction): Promise<void> {
    if (!nativeConnected) {
      message = 'The selection set needs the native graph runtime.'
      return
    }
    selectionBusyNode = nodeId
    failed = false
    try {
      const outcome = await applySelectionAction(nodeId, action)
      if (!outcome.ok) {
        failed = true
        message = outcome.error
      } else if (action === 'reselect') {
        message = `Selected ${outcome.count} element${outcome.count === 1 ? '' : 's'} in Archicad`
      } else {
        const verb = action === 'update' ? 'Updated' : action === 'add' ? 'Added' : action === 'remove' ? 'Removed' : 'Cleared'
        message = `${verb}: ${outcome.changed} this press, ${outcome.count} in the set`
        // Reported separately: the set can change correctly and the graph
        // downstream of it still fail, and one message for both would make a
        // successful capture read as a failed one.
        if (outcome.evaluationError !== '') {
          failed = true
          message += ` / downstream: ${outcome.evaluationError}`
        }
      }
    } catch (error) {
      failed = true
      message = error instanceof Error ? error.message : String(error)
    } finally {
      selectionBusyNode = null
      await refreshResults()
      await reloadState()
    }
  }

  // ---------------------------------------------------------------------
  // Stage F flow control.
  //
  // Both go through Tapioca.GraphApplyEdit, the SAME endpoint as a wire or a
  // parameter, because a mode change moves the document exactly as those do.
  // The browser never synthesizes the outcome: it asks, and then redraws
  // whatever the runtime says the graph now is.
  // ---------------------------------------------------------------------

  async function applyFlowControlEdit(params: Record<string, unknown>, describe: string): Promise<void> {
    if (!nativeConnected) {
      message = 'Flow control needs the native graph runtime.'
      return
    }
    busy = true
    failed = false
    try {
      await callTapioca('Tapioca.GraphApplyEdit', { graphId, ...params })
      await reloadState()
      await refreshResults()
      message = describe
    } catch (error) {
      // The runtime's refusal is shown as itself. It already names the reason -
      // "declares no bypass mapping", "is not a hold-capable node" - and
      // replacing that with a generic bridge failure is the exact behaviour the
      // handoff rules out.
      failed = true
      message = error instanceof Error ? error.message : String(error)
    } finally {
      busy = false
    }
  }

  async function setExecutionMode(nodeId: string, mode: ExecutionMode): Promise<void> {
    await applyFlowControlEdit({ editKind: 'setExecutionMode', nodeId, mode }, `${nodeId} is now ${mode}`)
  }

  async function releaseHolding(nodeId: string): Promise<void> {
    await applyFlowControlEdit({ editKind: 'releaseHolding', nodeId }, `Released ${nodeId}`)
  }

  // ---------------------------------------------------------------------
  // File menu. The runtime owns the graph and the library; everything here is
  // a request and a redraw from whatever it answers.
  // ---------------------------------------------------------------------

  /**
   * File > Save / File > Load: the ordinary Windows dialog, in the library folder.
   *
   * This is what a person expects from a File menu, and it is what File > Save
   * now does: the dialog opens on the workflow library, its file-name field IS
   * the workflow name, and the name comes back here to go through the same
   * save and load verbs the in-page dialog always used. The runtime stays
   * name-addressed and sandboxed - the chooser only picks a name, it never
   * hands the runtime a path.
   *
   * The in-page dialog is still the fallback, for the hosts that have no
   * chooser: a standalone browser, and the DG::Browser bridge, which does not
   * intercept the verb. Those reject the call, and rejecting is the signal.
   */
  async function openFileDialog(mode: LibraryMode): Promise<boolean> {
    let outcome
    try {
      outcome = await browseForGraph(mode, mode === 'save' ? currentGraphName : '')
    } catch {
      return false
    }
    if (outcome.status === 'cancelled') return true
    if (!outcome.ok) {
      failed = true
      message = outcome.error
      return true
    }
    if (mode === 'save') await performSave(outcome.name)
    else await performLoad(outcome.name)
    // performSave and performLoad report a refusal into the in-page dialog,
    // which is not open on this path. Promote it to the status line rather than
    // letting a failed save look like a silent one.
    if (libraryError !== '') {
      failed = true
      message = libraryError
      libraryError = ''
    }
    return true
  }

  async function openLibrary(mode: LibraryMode): Promise<void> {
    if (!nativeConnected) {
      message = 'The workflow library needs the native graph runtime.'
      return
    }
    if (await openFileDialog(mode)) return
    libraryError = ''
    libraryBusy = true
    libraryMode = mode
    try {
      const listing = await listGraphs()
      libraryGraphs = listing.graphs
      libraryLocation = listing.location
    } catch (error) {
      libraryError = error instanceof Error ? error.message : String(error)
    } finally {
      libraryBusy = false
    }
  }

  function closeLibrary(): void {
    libraryMode = null
    libraryError = ''
  }

  async function confirmLibrary(name: string): Promise<void> {
    if (libraryMode === 'save') await performSave(name)
    else await performLoad(name)
  }

  async function performSave(name: string): Promise<void> {
    libraryBusy = true
    libraryError = ''
    try {
      const outcome = await saveGraph(name, name, positions, nodes.map((node) => node.id))
      if (!outcome.ok) {
        libraryError = outcome.error
        return
      }
      saveAnnotations(localStorage, name, annotations)
      currentGraphName = name
      persistVisuals()
      closeLibrary()
      message = `Saved "${name}" to the workflow library / ${nodes.length} nodes`
      failed = false
    } catch (error) {
      libraryError = error instanceof Error ? error.message : String(error)
    } finally {
      libraryBusy = false
    }
  }

  async function performLoad(name: string): Promise<void> {
    libraryBusy = true
    libraryError = ''
    try {
      const outcome = await loadGraph(name)
      if (!outcome.ok) {
        // Reported in the dialog rather than as a graph failure: the graph on
        // screen is untouched, which is exactly what the runtime guarantees.
        libraryError = outcome.error
        return
      }
      // The runtime carried the layout; adopt it BEFORE redrawing so nodes land
      // where they were saved rather than on the automatic grid.
      positions.clear()
      applyLayoutToPositions(outcome.nodeLayout, positions)
      currentGraphName = name
      loadVisuals()
      annotations = loadAnnotations(localStorage, name)
      closeLibrary()
      busy = true
      try {
        await refreshResults()
        await reloadState()
        void fitView({ duration: 180, padding: 0.2 })
        message = `Loaded "${name}" / ${nodes.length} nodes / revision ${revision}`
        failed = false
      } finally {
        busy = false
      }
    } catch (error) {
      libraryError = error instanceof Error ? error.message : String(error)
    } finally {
      libraryBusy = false
    }
  }

  async function removeFromLibrary(name: string): Promise<void> {
    libraryBusy = true
    libraryError = ''
    try {
      const outcome = await deleteGraph(name)
      if (!outcome.ok) {
        libraryError = outcome.error
        return
      }
      libraryGraphs = libraryGraphs.filter((graph) => graph.name !== name)
      if (currentGraphName === name) currentGraphName = ''
    } catch (error) {
      libraryError = error instanceof Error ? error.message : String(error)
    } finally {
      libraryBusy = false
    }
  }

  async function newGraph(): Promise<void> {
    if (!nativeConnected) {
      nodes = []
      edges = []
      positions.clear()
      visuals.clear()
      persistVisuals()
      message = 'Cleared the browser-only fixture'
      return
    }
    if (nodes.length > 0 && !window.confirm(`Discard ${nodes.length} unsaved nodes and start a new graph?`)) {
      return
    }
    busy = true
    failed = false
    try {
      if (nodes.length > 0) {
        await callTapioca('Tapioca.GraphEraseElements', {
          nodeIds: nodes.map((node) => node.id),
          edges: [],
        })
      }
      positions.clear()
      visuals.clear()
      annotations = []
      currentGraphName = ''
      persistVisuals()
      persistAnnotations()
      message = 'New graph'
    } catch (error) {
      failed = true
      message = error instanceof Error ? error.message : String(error)
    } finally {
      await reloadState()
      await refreshResults()
      busy = false
    }
  }

  function handleFileAction(action: 'new' | 'load' | 'save'): void {
    if (action === 'new') void newGraph()
    else void openLibrary(action)
  }

  function rememberPosition({ targetNode }: { targetNode: Node<SchemaNodeData> | null }): void {
    if (targetNode !== null) positions.set(targetNode.id, { ...targetNode.position })
  }

  function editorMetadataKey(): string {
    return currentGraphName === '' ? graphId : currentGraphName
  }

  function loadEditorAnnotations(): void {
    annotations = loadAnnotations(localStorage, editorMetadataKey())
  }

  function persistAnnotations(): void {
    saveAnnotations(localStorage, editorMetadataKey(), annotations)
  }

  function createFrameFromSelection(): void {
    const frame = annotationFromSelection(
      `frame-${Date.now().toString(36)}`,
      nodes.filter((node) => node.selected),
    )
    if (frame === undefined) {
      message = 'Select one or more nodes before grouping.'
      return
    }
    annotations = [...annotations, frame]
    persistAnnotations()
    message = `Grouped ${frame.memberNodeIds.length} nodes in a visual frame / Ctrl+G`
  }

  // -------------------------------------------------------------------------
  // Annotations are presentation only: they live in this browser's storage and
  // the runtime never sees one. That is exactly why they need their own delete
  // and rename - nothing else in the editor can reach them, which is what made
  // a stray rectangle permanent.
  // -------------------------------------------------------------------------

  function selectedAnnotation(): EditorAnnotation | undefined {
    return resolvedAnnotations.find((annotation) => annotation.id === selectedAnnotationId)
  }

  function deleteAnnotation(id: string): void {
    const removed = resolvedAnnotations.find((annotation) => annotation.id === id)
    annotations = removeAnnotation(annotations, id)
    if (selectedAnnotationId === id) selectedAnnotationId = null
    persistAnnotations()
    message = `Deleted the ${removed?.kind ?? 'annotation'} "${removed?.label || 'untitled'}"`
  }

  function promptRenameAnnotation(id: string): void {
    const annotation = resolvedAnnotations.find((item) => item.id === id)
    if (annotation === undefined) return
    const label = window.prompt('Annotation label', annotation.label)
    if (label === null) return
    annotations = renameAnnotation(annotations, id, label)
    persistAnnotations()
    message = label.trim() === '' ? 'Cleared the annotation label' : `Renamed the annotation to "${label.trim()}"`
  }

  function clearAnnotations(): void {
    if (annotations.length === 0) return
    const count = annotations.length
    annotations = []
    selectedAnnotationId = null
    persistAnnotations()
    message = `Removed ${count} annotation${count === 1 ? '' : 's'}`
  }

  /** Which annotation, if any, a canvas press landed in. */
  function annotationAt(event: MouseEvent): EditorAnnotation | undefined {
    return annotationAtPoint(resolvedAnnotations, screenToFlowPosition({ x: event.clientX, y: event.clientY }))
  }

  function removeNodesFromFrames(removedNodeIds: Set<string>): void {
    if (removedNodeIds.size === 0) return
    annotations = annotations
      .map((annotation) =>
        annotation.kind === 'frame'
          ? {
              ...annotation,
              memberNodeIds: annotation.memberNodeIds.filter((nodeId) => !removedNodeIds.has(nodeId)),
            }
          : annotation,
      )
      .filter((annotation) => annotation.kind !== 'frame' || annotation.memberNodeIds.length > 0)
    persistAnnotations()
  }

  function setActiveTool(tool: EditorTool): void {
    activeTool = tool
    toolGesture = null
    annotationDraft = undefined
    clearErasePreview()
    message = tool === 'select' ? 'Selection tool' : tool === 'eraser' ? 'Eraser tool' : 'Rectangle tool'
  }

  function setTheme(value: ThemeMode): void {
    theme = value
    localStorage.setItem('tapioca.graph.theme', value)
  }

  function toggleSolutionLock(): void {
    solutionLocked = !solutionLocked
    failed = false
    message = solutionLocked
      ? 'Solution locked / edits are accepted, nothing is evaluated until you resume'
      : 'Solution resumed / Evaluate is available again'
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

  function handlePaneClick(event: MouseEvent): void {
    contextTarget = null
    const annotation = annotationAt(event)
    selectedAnnotationId = annotation?.id ?? null
    if (annotation !== undefined) {
      message = `Selected the ${annotation.kind} "${annotation.label || 'untitled'}" / Delete removes it, right-click renames it`
      failed = false
    }
  }

  function openPaneContext({ event }: { event: MouseEvent }): void {
    event.preventDefault()
    // Right-clicking an annotation selects it, so the menu that opens is about
    // the thing under the pointer rather than about the canvas in general.
    selectedAnnotationId = annotationAt(event)?.id ?? null
    contextTarget = { kind: 'pane', ...contextPosition(event) }
  }

  /**
   * Double-click anywhere: the component search, at the pointer, placing the
   * node where it was double-clicked. The rail is still the way to BROWSE the
   * catalog; this is the way to place something whose name you know.
   */
  function handlePaneDoubleClick(event: MouseEvent): void {
    if (!(event.target instanceof Element) || event.target.closest('.svelte-flow__pane') === null) return
    if (busy || effectiveTool !== 'select') return
    contextTarget = null
    const point = contextPosition(event)
    searchTarget = {
      ...point,
      position: screenToFlowPosition({ x: event.clientX, y: event.clientY }, { snapToGrid: snapEnabled }),
    }
  }

  function openNodeContext({ node, event }: { node: Node<SchemaNodeData>; event: MouseEvent }): void {
    event.preventDefault()
    contextTarget = { kind: 'node', node, ...contextPosition(event) }
  }

  function openPortContext(
    event: MouseEvent,
    target: { nodeId: string; portId: string; direction: 'input' | 'output' },
  ): void {
    contextTarget = { kind: 'port', ...target, ...contextPosition(event) }
  }

  function openEdgeContext({ edge, event }: { edge: Edge; event: MouseEvent }): void {
    event.preventDefault()
    contextTarget = { kind: 'edge', edge, ...contextPosition(event) }
  }

  function contextLabel(): string {
    if (contextTarget?.kind === 'port') {
      const port = contextPort()
      return port === undefined ? 'Port' : `${port.label} / ${port.valueType}`
    }
    if (contextTarget?.kind === 'node') return contextTarget.node.data.schema.label
    if (contextTarget?.kind === 'edge') return 'Connection'
    const annotation = selectedAnnotation()
    if (annotation !== undefined) return annotation.kind === 'frame' ? 'Frame' : 'Annotation'
    return 'Canvas'
  }

  function contextPort() {
    // Copied to a local first: narrowing does not survive into the callbacks
    // below, because `contextTarget` is reactive state and reads as a getter.
    const target = contextTarget
    if (target?.kind !== 'port') return undefined
    const node = nodes.find((item) => item.id === target.nodeId)
    const ports = target.direction === 'input' ? node?.data.schema.inputs : node?.data.schema.outputs
    return ports?.find((port) => port.portId === target.portId)
  }

  function contextActions() {
    // Ports go through the SAME menu as everything else. The transforms are
    // grouped rather than listed flat, because they are a different kind of
    // thing from the four verbs above them.
    if (contextTarget?.kind === 'port') {
      const { nodeId, portId, direction } = contextTarget
      const connected = edges.some((edge) =>
        direction === 'input'
          ? edge.target === nodeId && edge.targetHandle === portId
          : edge.source === nodeId && edge.sourceHandle === portId,
      )
      const native = 'Requires a native expected-revision graph edit'
      const transforms =
        direction === 'input'
          ? ['reverse', 'simplify', 'flatten', 'graft', 'reparameterize']
          : ['simplify', 'flatten', 'graft']
      return [
        { label: 'Copy reference', run: () => copyPortReference(nodeId, portId, direction) },
        ...(direction === 'input'
          ? [{ label: 'Paste reference', run: () => void pastePortReference(nodeId, portId) }]
          : []),
        {
          label: direction === 'input' ? 'Disconnect' : 'Disconnect all',
          disabled: !connected || busy,
          title: connected ? undefined : `Nothing is wired to this ${direction}.`,
          run: () => void disconnectPort(nodeId, portId, direction),
        },
        ...(direction === 'input'
          ? [
              { label: 'Internalise', disabled: true, title: native, run: () => {} },
              { label: 'Promote parameter', disabled: true, title: native, run: () => {} },
            ]
          : [
              { label: 'Promote as graph output', disabled: true, title: native, run: () => {} },
              { label: 'Inspect data', disabled: true, title: 'Requires native runtime value inspection', run: () => {} },
              { label: 'Set as display output', disabled: true, title: native, run: () => {} },
            ]),
        ...transforms.map((transform) => ({
          label: transform,
          disabled: true,
          title: native,
          group: 'Modifiers',
          run: () => {},
        })),
      ]
    }
    if (contextTarget?.kind === 'node') {
      const node = contextTarget.node
      const mode = node.data.executionMode ?? 'enabled'
      const schema = node.data.schema
      // Stage F. Capability comes from the CATALOG, so an action this node type
      // cannot support is greyed rather than offered and then refused - and the
      // reason travels with it, because a grey label with no explanation is the
      // thing the handoff explicitly rules out.
      const canBypass = (schema.bypassMappings?.length ?? 0) > 0
      const canHold = schema.holdCapable === true
      const modeAction = (label: string, target: ExecutionMode, allowed: boolean, why: string) => ({
        label: mode === target ? `${label} (current)` : label,
        disabled: busy || !nativeConnected || mode === target || !allowed,
        title: allowed ? undefined : why,
        run: () => void setExecutionMode(node.id, target),
      })
      return [
        { label: 'Quick actions', title: 'Also: middle-click the node', run: () => requestQuickMenu(node.id) },
        { label: 'Rename', title: 'Also: right-click the node title', run: () => requestRename(node.id) },
        { label: 'Fit node', run: () => void fitView({ nodes: [node], duration: 180, padding: 0.8 }) },
        modeAction('Enable', 'enabled', true, ''),
        modeAction('Disable', 'disabled', true, ''),
        modeAction(
          'Bypass',
          'bypassed',
          canBypass,
          `${schema.label} declares no bypass mapping, so there is no unambiguous input to forward.`,
        ),
        modeAction('Hold', 'holding', canHold, `${schema.label} is not a hold-capable node.`),
        {
          label: 'Release held value',
          disabled: busy || !nativeConnected || mode !== 'holding',
          run: () => void releaseHolding(node.id),
        },
        { label: 'Delete node', disabled: busy, run: () => void removeElements([node], []) },
      ]
    }
    if (contextTarget?.kind === 'edge') {
      const edge = contextTarget.edge
      return [{ label: 'Disconnect', disabled: busy, run: () => void removeElements([], [edge]) }]
    }
    // The canvas menu. An annotation under the pointer puts its own two verbs at
    // the top rather than opening a different menu: it IS the canvas menu, for
    // the piece of canvas that was clicked.
    const annotation = selectedAnnotation()
    return [
      ...(annotation === undefined
        ? []
        : [
            { label: `Rename "${annotation.label || 'untitled'}"`, run: () => promptRenameAnnotation(annotation.id) },
            { label: `Delete ${annotation.kind}`, run: () => deleteAnnotation(annotation.id) },
          ]),
      {
        label: solutionLocked ? 'Resume solution' : 'Lock solution',
        title: solutionLocked
          ? 'Allow Evaluate to run again'
          : 'Keep editing without evaluating; Evaluate is refused until you resume',
        run: toggleSolutionLock,
      },
      {
        label: pickerOpen ? 'Close components catalog' : 'Open components catalog',
        run: () => (pickerOpen = !pickerOpen),
      },
      { label: 'Fit graph', disabled: nodes.length === 0, run: () => void fitView({ duration: 180, padding: 0.2 }) },
      {
        label: 'Clear annotations',
        disabled: annotations.length === 0,
        title: annotations.length === 0 ? 'This graph has no rectangles or frames.' : undefined,
        run: clearAnnotations,
      },
    ]
  }

  function isToolUi(target: EventTarget | null): boolean {
    return (
      target instanceof Element &&
      target.closest('.component-picker, .component-rail, .svelte-flow__controls, .svelte-flow__minimap') !== null
    )
  }

  function addEraseHitsAt(clientX: number, clientY: number, knifeOnly: boolean): void {
    const nextNodes = new Set(erasedNodeIds)
    const nextEdges = new Set(erasedEdgeIds)
    for (const element of document.elementsFromPoint(clientX, clientY)) {
      const edge = element.closest<HTMLElement>('.svelte-flow__edge[data-id]')
      if (edge !== null) nextEdges.add(edge.dataset.id ?? '')
      if (!knifeOnly) {
        const node = element.closest<HTMLElement>('.svelte-flow__node[data-id]')
        if (node !== null) nextNodes.add(node.dataset.id ?? '')
      }
    }
    nextNodes.delete('')
    nextEdges.delete('')
    erasedNodeIds = [...nextNodes]
    erasedEdgeIds = [...nextEdges]
    syncErasePreview()
  }

  function addEraseHitsAlong(start: XYPosition, end: XYPosition, knifeOnly: boolean): void {
    const distance = Math.hypot(end.x - start.x, end.y - start.y)
    const steps = Math.max(1, Math.ceil(distance / 6))
    for (let index = 0; index <= steps; index += 1) {
      const amount = index / steps
      addEraseHitsAt(
        start.x + (end.x - start.x) * amount,
        start.y + (end.y - start.y) * amount,
        knifeOnly,
      )
    }
  }

  function syncErasePreview(): void {
    const nodeIds = new Set(erasedNodeIds)
    const edgeIds = new Set(erasedEdgeIds)
    nodes = nodes.map((node) => ({ ...node, class: nodeIds.has(node.id) ? 'erase-preview' : undefined }))
    edges = edges.map((edge) => ({ ...edge, class: edgeIds.has(edge.id) ? 'erase-preview' : undefined }))
  }

  function clearErasePreview(): void {
    erasedNodeIds = []
    erasedEdgeIds = []
    nodes = nodes.map((node) => ({ ...node, class: undefined }))
    edges = edges.map((edge) => ({ ...edge, class: undefined }))
  }

  function handleToolPointerDown(event: PointerEvent): void {
    if (busy || effectiveTool === 'select' || isToolUi(event.target)) return
    contextTarget = null
    if (effectiveTool === 'rectangle') {
      if (!(event.target instanceof Element) || event.target.closest('.svelte-flow__pane') === null) return
      const start = screenToFlowPosition({ x: event.clientX, y: event.clientY }, { snapToGrid: snapEnabled })
      toolGesture = { kind: 'rectangle', pointerId: event.pointerId, start }
      annotationDraft = {
        id: 'rectangle-draft',
        kind: 'rectangle',
        bounds: boundsFromPoints(start, start),
        label: '',
        memberNodeIds: [],
      }
    } else {
      const point = { x: event.clientX, y: event.clientY }
      toolGesture = { kind: 'erase', pointerId: event.pointerId, last: point }
      addEraseHitsAt(event.clientX, event.clientY, effectiveTool === 'knife')
    }
    canvas?.setPointerCapture(event.pointerId)
    event.preventDefault()
    event.stopPropagation()
  }

  function handleToolPointerMove(event: PointerEvent): void {
    if (toolGesture === null || event.pointerId !== toolGesture.pointerId) return
    if (toolGesture.kind === 'rectangle') {
      const end = screenToFlowPosition({ x: event.clientX, y: event.clientY }, { snapToGrid: snapEnabled })
      annotationDraft = {
        id: 'rectangle-draft',
        kind: 'rectangle',
        bounds: boundsFromPoints(toolGesture.start, end),
        label: '',
        memberNodeIds: [],
      }
    } else {
      const end = { x: event.clientX, y: event.clientY }
      addEraseHitsAlong(toolGesture.last, end, effectiveTool === 'knife')
      toolGesture = { ...toolGesture, last: end }
    }
    event.preventDefault()
  }

  function handleToolPointerUp(event: PointerEvent): void {
    if (toolGesture === null || event.pointerId !== toolGesture.pointerId) return
    const completed = toolGesture
    toolGesture = null
    if (canvas?.hasPointerCapture(event.pointerId)) canvas.releasePointerCapture(event.pointerId)

    if (completed.kind === 'rectangle') {
      const draft = annotationDraft
      annotationDraft = undefined
      if (draft !== undefined && draft.bounds.width >= 8 && draft.bounds.height >= 8) {
        annotations = [
          ...annotations,
          { ...draft, id: `rectangle-${Date.now().toString(36)}`, label: 'Annotation' },
        ]
        persistAnnotations()
        message = 'Created presentation-only rectangle.'
      }
    } else {
      const selectedNodes = nodes.filter((node) => erasedNodeIds.includes(node.id))
      const selectedEdges = edges.filter((edge) => erasedEdgeIds.includes(edge.id))
      clearErasePreview()
      void removeElements(selectedNodes, selectedEdges)
    }
    event.preventDefault()
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

  <svelte:window onkeydown={handleKeyDown} onkeyup={handleKeyUp} />

<main data-theme={theme}>
  <header class="toolbar">
    <div class="brand">
      <span>Tapioca / Experimental</span>
      <strong>Node Graph</strong>
    </div>
    <ToolStrip tool={activeTool} {effectiveTool} disabled={busy} onchange={setActiveTool} />
    <ApplicationMenu
      {busy}
      {nativeConnected}
      {snapEnabled}
      {theme}
      {performanceOpen}
      onrefresh={() => void reloadState()}
      onevaluate={() => void evaluate()}
      onevaluatesequential={() => void evaluate(1)}
      {solutionLocked}
      ontogglelock={toggleSolutionLock}
      onfileaction={handleFileAction}
      onfit={() => void fitView({ duration: 180, padding: 0.2 })}
      ontogglesnap={toggleSnap}
      ontheme={setTheme}
      ontoggleperformance={() => (performanceOpen = !performanceOpen)}
    />
    <div class="actions">
      <button onclick={removeSelection} disabled={busy}>Erase selected</button>
    </div>
  </header>

  {#if libraryMode !== null}
    <LibraryDialog
      mode={libraryMode}
      graphs={libraryGraphs}
      location={libraryLocation}
      busy={libraryBusy}
      error={libraryError}
      suggestedName={currentGraphName}
      onconfirm={(name) => void confirmLibrary(name)}
      ondelete={(name) => void removeFromLibrary(name)}
      oncancel={closeLibrary}
    />
  {/if}

  <section
    class="canvas"
    class:picker-open={pickerOpen}
    class:tool-eraser={effectiveTool === 'eraser'}
    class:tool-rectangle={effectiveTool === 'rectangle'}
    class:tool-knife={effectiveTool === 'knife'}
    aria-label="Node graph canvas"
    data-detail-level={detailLevel}
    onpointerdown={handleToolPointerDown}
    onpointermove={(event) => {
      movePlainMarker(event)
      handleToolPointerMove(event)
    }}
    onpointerup={handleToolPointerUp}
    onpointercancel={handleToolPointerUp}
    ondblclick={handlePaneDoubleClick}
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
      panOnDrag={effectiveTool === 'select' ? PAN_BUTTONS : false}
      selectionOnDrag={effectiveTool === 'select'}
      panOnScroll={false}
      zoomOnDoubleClick={false}
      nodesDraggable={effectiveTool === 'select'}
      nodesConnectable={effectiveTool === 'select'}
      elementsSelectable={effectiveTool === 'select'}
      {isValidConnection}
      onbeforeconnect={requestConnection}
      onbeforedelete={removeFromRuntime}
      onnodedragstop={rememberPosition}
      onmove={handleMove}
      ondragover={handleDragOver}
      ondrop={handleDrop}
      onpaneclick={({ event }) => handlePaneClick(event)}
      onpanecontextmenu={openPaneContext}
      onnodecontextmenu={openNodeContext}
      onedgecontextmenu={openEdgeContext}
      deleteKey={['Backspace', 'Delete']}
      >
        <AnnotationLayer annotations={resolvedAnnotations} draft={annotationDraft} selectedId={selectedAnnotationId ?? undefined} />
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

    {#if searchTarget !== null}
      <NodeSearch
        {catalog}
        x={searchTarget.x}
        y={searchTarget.y}
        onplace={(nodeType) => void addNode(nodeType, searchTarget?.position)}
        onclose={() => (searchTarget = null)}
      />
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
    {#if solutionLocked}<code class="locked">solution locked</code>{/if}
    <code>{nativeConnected ? `rev ${revision}` : 'fixture'}</code>
  </footer>
</main>
