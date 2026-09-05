import type { XYPosition } from '@xyflow/svelte'
import type { NodeVisualState } from './nodes/types/node'
import type { PortSchema } from './nodes/types/port'
import type { ExecutionMode, RuntimeStatus } from './nodes/types/runtime'
import type { PortConnectionState } from './nodes/types/port'
import type { ComponentMessage } from './nodes/types/diagnostics'
import type { PortReference } from './nodes/types/portReference'

export type { NodeBodyMode, NodeVisualState } from './nodes/types/node'
export type { PortDirection, PortSchema, PortTransform } from './nodes/types/port'
export type { ExecutionMode, RuntimeStatus } from './nodes/types/runtime'
export type { PortReference } from './nodes/types/portReference'

/**
 * UI-1. How the runtime says a parameter is EDITED.
 *
 * A projection of the native `ParameterUi`, never a definition authored here:
 * `widget` is a closed enum the runtime validates at registration, not a Svelte
 * component name, so adding a node type or a parameter needs no change in this
 * package. See docs/architecture/api/HANDOFF-NodeGraphUIBuilder.md section 4.
 *
 * Every field is a DISPLAY or INPUT hint. None of it changes what the parameter
 * means: the node clamps and rounds its own value, so a control that ignored a
 * range would produce a wrong-looking box, never a wrong answer.
 */
export type ParameterWidget =
  | 'auto'
  | 'number'
  | 'slider'
  | 'boolean'
  | 'select'
  | 'text'
  | 'vector'
  | 'point'
  | 'color'
  | 'readOnly'
  | 'previewTarget'
  | 'libraryPart'

/**
 * Where a select's choices come from when they are not literal.
 *
 * ⚠️ NOTHING HERE ENUMERATES A MODEL DOMAIN. The layer list belongs to the open
 * project, so it is neither static catalog data nor the browser's to compute -
 * the parameter names the domain and the native attribute listing answers with
 * the members.
 */
export type ParameterOptionSource =
  | 'none'
  | 'layer'
  | 'pen'
  | 'fill'
  | 'lineType'
  | 'surface'
  | 'buildingMaterial'
  | 'composite'
  | 'profile'

export interface ParameterOption {
  label: string
  value: GraphValue
}

export interface ParameterUi {
  widget: ParameterWidget
  section: string
  order: number
  help: string
  /** Drawn after the field - "mm". Presentation only; the value is in runtime units. */
  unit: string
  minimum?: number
  maximum?: number
  step?: number
  decimals?: number
  /**
   * A SIBLING PARAMETER supplying the live bound instead of the constant above.
   * This is what makes a slider whose range and precision the user controls
   * expressible without this package knowing which node it is looking at.
   */
  minimumParameter?: string
  maximumParameter?: string
  stepParameter?: string
  decimalsParameter?: string
  components: string[]
  options: ParameterOption[]
  optionSource: ParameterOptionSource
}

/**
 * An attribute's swatch, as a DEFINITION rather than an image.
 *
 * Eight bytes of bit pattern, a dash sequence, a stack of skin thicknesses: the
 * runtime sends what the attribute IS and the client draws it. See
 * AttributeSwatch.svelte for why that split is the useful one.
 */
export interface AttributePreview {
  kind: 'color' | 'pattern' | 'line' | 'composite' | 'surface'
  color?: string
  /** Surfaces only: whether the surface carries a texture image. */
  hasTexture?: boolean
  fillKind?: 'vector' | 'symbol' | 'solid' | 'empty' | 'linearGradient' | 'radialGradient' | 'image'
  /** Eight rows of eight bits, one byte each, high bit leftmost. */
  pattern?: number[]
  lineKind?: 'solid' | 'dashed' | 'symbol'
  dashes?: number[]
  thickness?: number
  skins?: { thickness: number; color?: string }[]
}

/** One row of the native attribute listing. */
export interface AttributeRow {
  label: string
  name?: string
  number?: number
  index: number
  color?: string
  hidden?: boolean
  locked?: boolean
  /** Slash-joined folder path; absent means the attribute sits at the root. */
  folder?: string
  preview?: AttributePreview
}

export interface ParameterSchema {
  parameterId: string
  label: string
  valueType: string
  required: boolean
  defaultValue?: GraphValue
  /** Absent is valid: every parameter registered before UI-1 has none. */
  ui?: ParameterUi
}

/**
 * Stage F3. One input forwarded to one output while the node is bypassed.
 *
 * Declared by the node TYPE, never inferred here: a node with two compatible
 * inputs has no derivable answer, and a client that guessed would make bypass
 * mean something different from what the runtime does.
 */
export interface BypassMapping {
  inputId: string
  outputId: string
}

/**
 * Stage F. What the user has told the graph to do with a node.
 *
 * Not a status. A mode is authored and persists with the graph; a status is
 * produced by a run and is session-only. See NodeResultRecord['status'].
 */
export interface NodeTypeSchema {
  nodeType: string
  label: string
  category: string
  description: string
  executionDomain: string
  effect?: string
  /**
   * How the runtime suggests this node's body is drawn: 'ports' (default),
   * 'text' (show the node's text output, the Grasshopper-panel shape), or
   * 'preview'. Driven by the catalog rather than by branching on nodeType here,
   * so a new inspectable node needs no change in this file.
   */
  display?: 'ports' | 'text' | 'preview' | 'selectionSet' | 'script'
  generations?: string[]
  /**
   * Empty means this type cannot be bypassed. The picker and context menu grey
   * the action from this rather than attempting the edit and reporting the
   * refusal - an action offered and then refused is worse than one never
   * offered.
   */
  bypassMappings?: BypassMapping[]
  /** Whether ExecutionMode 'holding' is legal for this type. */
  holdCapable?: boolean
  /**
   * This type's nodes carry their OWN ports, and its `inputs` and `outputs`
   * below are empty as a result. That is not a catalog bug: a script node's
   * interface is declared in the file it runs, so the catalog has nothing to say
   * about it and the per-node state carries it instead.
   *
   * ⚠️ NEVER READ `inputs`/`outputs` OFF THE CATALOG FOR A NODE YOU HAVE IN
   * HAND. Go through schemaForNode, which merges the two. A reader that skips it
   * draws every script node with no ports at all - and it looks like the node is
   * broken rather than like the code is.
   */
  instancePorts?: boolean
  inputs: PortSchema[]
  outputs: PortSchema[]
  parameters: ParameterSchema[]
}

/**
 * One row of the loaded-library catalogue, as Tapioca.ListLibraryParts reports
 * it.
 *
 * ⚠️ THIS IS THE PLACEABLE CATALOGUE, NOT EVERY REGISTERED PART. The native side
 * narrows it: an unset subtype means GDL OBJECTS, the same list Archicad's own
 * Object Settings browser shows. The first cut listed everything and put
 * surfaces, images, section markers and templates in front of somebody asking
 * "which object do I place" - reported as *"data is all over the place"*.
 */
export interface LibraryPartRow {
  /** What the Object tool's dialog shows. NOT an identity - see `unID`. */
  name: string
  /** The .gsm on disk. */
  file: string
  /**
   * The stable cross-session key. A document name is unique only in that
   * Archicad registers the newest part carrying it, so two loaded libraries
   * shipping the same name leave one invisible; this is what survives.
   */
  unID: string
  type: string
  location: string
  placeable: boolean
  /** Registered, but its .gsm is gone. */
  missing: boolean
  /** The Library Manager's own folders, root first. */
  treePath: string[]
  library: string
  embedded: boolean
}

export interface LibraryCatalog {
  parts: LibraryPartRow[]
  /** Matches BEFORE the cap, so a truncated list still says how many there are. */
  total: number
  truncated: boolean
  /** Set when the listing failed; the picker shows it instead of an empty tree. */
  error?: string
}

/**
 * One part's preview picture.
 *
 * ⚠️ NO PICTURE IS A NORMAL ANSWER, NOT A FAILURE, and `reason` says which of
 * the three it was: the part has no preview section, its preview is a format no
 * browser draws (a real Archicad library ships TIFF), or it is over the transfer
 * cap. A grid that treated any of those as an error would report errors for a
 * large fraction of a perfectly healthy library.
 */
export interface LibraryPartPreview {
  name: string
  previewMime: string
  previewBytes: number
  /** Empty exactly when `reason` is not. */
  dataUri: string
  reason: string
}

/** The runtime's self-describing value encoding. */
export interface GraphValue {
  valueType: string
  bool?: boolean
  number?: number
  text?: string
  numbers?: number[]
  /**
   * A mesh's triangles, into `numbers` read as xyz triples. Present only on a
   * top-level mesh within the encoding cap; a mesh inside a list, or one past the
   * cap, arrives as counts with `truncated`.
   */
  indices?: number[]
  itemCount?: number
  truncated?: boolean
  items?: GraphValue[]
}

export interface GraphParameter {
  parameterId: string
  value?: GraphValue
  /** @deprecated the runtime still accepts this; new code sends `value`. */
  numberValue?: number
}

/**
 * One input port's modifier - what the port does to what arrives, before the
 * node sees it.
 *
 * The reshaping three are the same operations as the `tree.*` nodes that share
 * their names, applied at the port instead of on the canvas. `round` is the odd
 * one: it converts, so it is what lets a Double reach a port that declared
 * Integer.
 */
export interface PortModifierRecord {
  portId: string
  modifier: 'flatten' | 'graft' | 'simplify' | 'reverse' | 'round' | 'normalise'
}

export interface GraphNodeRecord {
  nodeId: string
  nodeType: string
  /** Only the ports that carry one; absent means the node modifies nothing. */
  inputModifiers?: PortModifierRecord[]
  /**
   * Stage F, and it arrives with the STATE rather than with the results, because
   * it is document state: a client that read it from the per-run results would
   * lose every disabled node the moment the run cache was dropped.
   */
  executionMode?: ExecutionMode
  parameters: GraphParameter[]
  /**
   * Present only on nodes whose TYPE sets `instancePorts` - see there. Absent on
   * every other node, which is how a client tells "this node authors its own
   * ports and currently has none" (a script whose header would not parse) from
   * "ask the catalog".
   */
  inputs?: PortSchema[]
  outputs?: PortSchema[]
}

export interface GraphEdgeRecord {
  sourceNode: string
  sourcePort: string
  targetNode: string
  targetPort: string
}

export interface GraphState {
  revision: number
  graphId?: string
  lastRunId?: number
  lastEventSeq?: number
  nodes: GraphNodeRecord[]
  edges: GraphEdgeRecord[]
}

/**
 * One branch of a published tree.
 *
 * ⚠️ `value` IS ALWAYS A LIST, even for a branch holding one item - a branch is
 * a list, and collapsing it to a scalar would erase the shape this record
 * exists to report. The whole-output `value` does collapse that way; the two
 * are deliberately different.
 */
export interface OutputBranch {
  /** The path a person reads, e.g. `{0;1}`. */
  path: string
  /** The same path to sort and group by, without parsing `path` back. */
  segments: number[]
  /** Items really in this branch, counted BEFORE the cap. */
  itemCount: number
  truncated: boolean
  value: GraphValue
}

export interface NodeOutputRecord {
  portId: string
  /**
   * The whole tree FLATTENED, in canonical order, and a one-item tree as that
   * item. What every client got before branches existed, and still the right
   * thing for anything that wants a value rather than a shape.
   */
  value: GraphValue
  /** The value rendered by the runtime, ready to display. */
  text: string
  /** A short label: "List of 12", "Element", "Number". */
  summary: string
  /**
   * The port's declared item type, e.g. `mesh`.
   *
   * NOT derivable from `value`: an empty tree of meshes and an empty tree of
   * numbers both project to an empty list.
   */
  itemType?: string
  /**
   * The tree's SHAPE. Twelve walls flat and four walls on each of three storeys
   * are the same `value` and different trees, and only these tell them apart.
   *
   * Optional because a runtime older than the tree layer sends neither; a client
   * reading only `value` is told nothing false, only less.
   */
  branchCount?: number
  branchesTruncated?: boolean
  branches?: OutputBranch[]
}

export interface NodeResultRecord {
  nodeId: string
  /**
   * Stage F1's public vocabulary. `dirty`, `complete`, `failed` and `skipped`
   * were the old internal spellings and are gone, not aliased.
   */
  status: RuntimeStatus
  /**
   * The stable machine-readable reason, e.g. `node.blocked.sideEffectsWithheld`.
   *
   * ⚠️ BRANCH ON THIS, NEVER ON `message`. Several distinct situations share one
   * status - a withheld side effect, an upstream disabled node and an unreleased
   * dam are all `blocked` - and only the code tells them apart. `message` is
   * prose for a person and may be reworded at any time.
   */
  code?: string
  message: string
  durationMilliseconds: number
  itemCount: number
  cacheHit?: boolean
  evaluationCount?: number
  runId?: number
  previewAvailable: boolean
  /** What the node actually produced. This is what makes results visible. */
  outputs?: NodeOutputRecord[]
}

/**
 * What one evaluation's levels actually overlapped.
 *
 * ADR-007's gate is that pure nodes DEMONSTRABLY execute concurrently, and
 * `peakConcurrency` is the demonstration: it is counted by the node bodies
 * themselves, not inferred from the timings, so it cannot be produced by fast
 * sequential execution.
 */
export interface ParallelismMetrics {
  workerThreads: number
  maxParallel: number
  peakConcurrency: number
  wallClockMs: number
  workMs: number
  /** Work divided by wall clock. 1.0 means nothing overlapped. */
  speedup: number
  levels: {
    levelIndex: number
    executedCount: number
    workerNodeCount: number
    hostNodeCount: number
    peakConcurrency: number
    wallClockMs: number
    workMs: number
  }[]
}

export interface EvaluationSummary {
  graphId: string
  runId: number
  revision: number
  succeeded: boolean
  cancelled: boolean
  error: string
  failedNode: string
  executedCount: number
  cacheHitCount: number
  failedCount: number
  blockedCount: number
  /**
   * Whether the run actually committed its host effects.
   *
   * ⚠️ FALSE IS NOT A FAILURE, AND MUST NOT BE READ AS SUCCESS EITHER. A run that
   * did not ask for side effects reports every effectful node as skipped and
   * still succeeds - which is exactly what every automatic pass does. Only a
   * deliberate press asks, and only then does this distinguish "sent" from
   * "the runtime declined to send".
   */
  effectsCommitted?: boolean
  /** The effectful nodes this run reported rather than performed. */
  skippedEffectNodes?: string[]
  parallelism?: ParallelismMetrics
}

export type SelectionAction = 'update' | 'add' | 'remove' | 'reselect' | 'clear'

export interface SelectionActionOutcome {
  ok: boolean
  error: string
  nodeId: string
  action: SelectionAction
  count: number
  changed: number
  missing: string[]
  revision: number
  evaluated: boolean
  executedCount: number
  /** Separate from `error`: the set can change correctly and the graph downstream still fail. */
  evaluationError: string
}

/**
 * One domain's whole answer.
 *
 * More than the rows, because a pen listing also carries the project's pen SETS
 * - the picker draws its set dropdown from the same response that carried the
 * pens, rather than from a second round trip that could disagree with it about
 * which set is showing.
 */
export interface AttributeListing {
  attributes: AttributeRow[]
  penSets?: string[]
  /** Which set these pens came from; absent means the project's current one. */
  penSet?: string
}

export interface SchemaNodeData extends Record<string, unknown> {
  schema: NodeTypeSchema
  parameters: GraphParameter[]
  /**
   * Stage F. Carried on the node rather than read from `result`, so a node stays
   * visibly disabled BETWEEN runs - when there is no result to read at all.
   */
  executionMode?: ExecutionMode
  /** Only the ports that carry one; see PortModifierRecord. */
  inputModifiers?: PortModifierRecord[]
  result?: NodeResultRecord
  /**
   * Present on selection-set nodes. The node draws its own five buttons, so the
   * handler has to reach it - and it comes through `data` because that is the
   * only channel Svelte Flow gives a custom node.
   */
  onselectionaction?: (nodeId: string, action: SelectionAction) => void
  /** True while an action on THIS node is in flight. */
  selectionBusy?: boolean
  /**
   * Present on nodes that perform a host effect. Automatic evaluation never
   * commits one - `allowSideEffects` defaults to refused - so this press is the
   * deliberate act that does, and it is per node rather than graph-wide: two
   * effectful nodes in one graph are two decisions.
   */
  onexecute?: (nodeId: string) => void
  /** True while THIS node's effect is being committed. */
  executeBusy?: boolean
  /**
   * What this node's viewer should draw.
   *
   * ⚠️ RESOLVED BY THE EDITOR, NOT READ OFF THE NODE'S OWN RESULT, because a
   * Preview node has NO OUTPUTS - it is a terminal, and its geometry lives on the
   * node upstream of it. The editor follows the same wire the runtime's preview
   * projection does, so the node's viewport and the Archicad overlay cannot
   * disagree about what this node is showing.
   */
  viewerValues?: GraphValue[]
  /** Presentation metadata. It is never sent to the evaluator. */
  visual?: NodeVisualState
  onvisualchange?: (nodeId: string, visual: NodeVisualState) => void
  onexecutionchange?: (nodeId: string, mode: ExecutionMode) => void
  /**
   * A value typed into a control. The node hands over TEXT and the port's
   * declared type; turning that into the runtime's value encoding is the
   * editor's job, because only the editor knows what the runtime accepts.
   */
  onparameterchange?: (nodeId: string, parameterId: string, valueType: string, text: string) => void
  /**
   * Which graph this node is in. Present on script nodes, whose panel calls
   * native verbs directly rather than going through the editor - and a verb that
   * defaulted to "the default graph" would act on the wrong document the moment
   * a second one existed.
   */
  graphId?: string
  /**
   * A script node reloaded and may have reshaped itself. The editor refetches the
   * graph state, because a script node's PORTS live in that state - the panel
   * cannot apply them itself.
   */
  onscriptreloaded?: () => void
  /**
   * Open this script node's file in the Script Inspector. The editor owns the
   * inspector rather than the node: a buffer someone is typing into cannot live
   * in a node body that unmounts the moment the node is panned off screen.
   */
  onscriptedit?: (nodeId: string) => void
  /** A port reference pasted onto one of this node's inputs: a connection request. */
  onportreference?: (reference: PortReference, target: { nodeId: string; portId: string }) => void
  oncopyportreference?: (nodeId: string, portId: string, direction: 'input' | 'output') => void
  onpasteportreference?: (nodeId: string, portId: string) => void
  /**
   * A right-click on one of this node's ports. The editor answers with THE
   * context menu; the port draws none of its own.
   */
  onportcontextmenu?: (
    event: MouseEvent,
    target: { nodeId: string; portId: string; direction: 'input' | 'output' },
  ) => void
  /**
   * What the native attribute listing answered, keyed by option source.
   *
   * Passed IN rather than fetched by the control, because a presentational
   * component must not call the bridge - and because one project-wide listing
   * serves every picker on the canvas instead of one call per node.
   *
   * The RAW rows, not finished options: turning a row into an option needs the
   * parameter's value type (a pen is picked by number, everything else by name),
   * and the row also carries the swatch, folder and layer flags the picker
   * draws. Converting here would throw all of that away.
   */
  attributeListings?: Record<string, AttributeListing>
  /** A select with nothing to show asking for its domain to be listed. */
  onrequestoptions?: (source: ParameterOptionSource, penSet?: string) => void
  /**
   * The loaded-library catalogue, and one part's thumbnail on request.
   *
   * Passed IN for the same reason the attribute listings are: a presentational
   * control must not call the bridge, and ONE listing serves every picker on the
   * canvas instead of one enumeration per node. `undefined` is "never asked".
   */
  libraryCatalog?: LibraryCatalog
  onrequestlibrary?: () => void
  /**
   * ⚠️ ONE PART PER CALL, AND ONLY FOR CELLS ON SCREEN. Reading a preview
   * touches the library file; a grid that fetched a four-thousand-part catalogue
   * through this would be unusable and would deserve to be.
   */
  onlibrarypreview?: (name: string) => Promise<LibraryPartPreview>
  /**
   * The per-type containers a selection-set node stacks under its buttons.
   *
   * ⚠️ RESOLVED FROM THE NODE'S OWN STORED CAPTURE, not from a host call. The
   * runtime records each element's type at the moment the user pressed a button,
   * so the stack draws offline, from the document, with no round trip - and it
   * is as old as that press, which is the same staleness the set itself has.
   */
  elementGroups?: ElementGroup[]
  /** A container asking for the live settings of what it holds. */
  ondescribeelements?: (guids: string[]) => Promise<ElementDescriptionResponse>
  portConnections?: PortConnectionState[]
  messages?: ComponentMessage[]
}

/**
 * One row of the classification-sensitive settings tree, as the runtime
 * describes it.
 *
 * ⚠️ THE SCHEMA COMES WITH THE ANSWER, IT IS NOT CACHED HERE. GraphDescribeElements
 * returns the descriptors for exactly the types present in the same response, so
 * this file never carries a copy of what a wall is - which is the copy that goes
 * stale after a build and starts showing a thickness under the label "Height".
 */
export interface ElementSettingSchema {
  id: string
  label: string
  /** 'Identity' | 'Placement' | 'Geometry' | 'Structure' | 'Display'. */
  group: string
  valueType: string
  /** The unit of the VALUE, not a decoration: an angle marked deg IS in degrees. */
  unit: string
  /**
   * 'archicad' when the value is a field of an ACAPI struct, 'derived' when
   * Tapioca computed it.
   *
   * ⚠️ WORTH SHOWING. Archicad has no wall length - it has two endpoints and an
   * angle - so a Length row that disagrees with a schedule is a disagreement
   * about definition, not a reader bug, and the user can only tell which if the
   * row says where the number came from.
   */
  origin: string
  /**
   * The sibling setting this row is conditional on, empty when it always
   * applies, together with the text that sibling must render as.
   *
   * ⚠️ THIS IS WHAT SEPARATES "DOES NOT APPLY" FROM "NOT READ". A Basic wall has
   * no composite, so the reader writes none; without the condition the panel
   * would report it as a field this build cannot read yet, which tells the user
   * the panel is short when it is complete. See `settingSectionsOf`.
   */
  appliesWhenSetting: string
  appliesWhenEquals: string
}

export interface ElementTypeSchema {
  id: string
  label: string
  plural: string
  settings: ElementSettingSchema[]
}

export interface ElementSettingValue {
  id: string
  text: string
  hasNumber: boolean
  number: number
}

export interface ElementDescription {
  guid: string
  elementType: string
  /** Archicad's own localised name for the type. */
  typeLabel: string
  /** False when the element could not be read; `detail` says why. */
  available: boolean
  detail: string
  /**
   * In the runtime table's order, and only the settings it could actually read.
   * A setting missing here is UNREAD, which is a different fact from zero.
   */
  settings: ElementSettingValue[]
}

export interface ElementDescriptionResponse {
  ok: boolean
  error: string
  truncated: boolean
  types: ElementTypeSchema[]
  elements: ElementDescription[]
}

/** One container in a selection node's stack: a type, and what it holds. */
export interface ElementGroup {
  elementType: string
  label: string
  guids: string[]
}

export type PositionStore = Map<string, XYPosition>
