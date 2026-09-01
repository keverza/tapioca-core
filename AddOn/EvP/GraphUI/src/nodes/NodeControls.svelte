<script lang="ts">
  /**
   * The node body's rows: one per input port, then one per parameter.
   *
   * The row SKELETON is fixed by the master template and does not vary by node -
   * a handle and a label on the left, the control in the middle, the unit or the
   * value type on the right. What varies is only which control lands in the
   * middle cell, and that comes from the runtime's widget descriptor through
   * ParameterControl. Nothing in this file branches on `nodeType`, and nothing
   * below it may either.
   *
   * THE FIRST SECTION IS THE NODE; THE REST IS ITS SETTINGS. A node's headline
   * control - the slider, the toggle, the picker - is what the user came for,
   * and the parameters that shape it (a range, a precision) are detail they set
   * once. So sections after the first collapse behind one chevron at the foot of
   * the node, expanded from the same descriptor `section` key the runtime
   * already sends. A node that declares one section has no chevron and loses
   * nothing.
   */
  import type { AttributeListing, GraphParameter, NodeOutputRecord, ParameterOptionSource } from '../types'
  import type { NodeDefinition } from './types/node'
  import type { PortConnectionState, PortLayout } from './types/port'
  import type { ComponentMessage } from './types/diagnostics'
  import type { PortReference } from './types/portReference'
  import NodePort from './NodePort.svelte'
  import ParameterControl from './controls/ParameterControl.svelte'
  import { portStructure } from './types/display'
  import { fieldsFor, sectionsFor, shouldShowSectionHeadings, withDefaults } from './controls/widgets'

  let { nodeId, definition, parameters, outputs, layout, connections, messages = [], attributeListings = {}, onparameter, onreference, onportmenu, onrequestoptions }: { nodeId: string; definition: NodeDefinition; parameters: GraphParameter[]; outputs?: NodeOutputRecord[]; layout: PortLayout; connections: PortConnectionState[]; messages?: ComponentMessage[]; attributeListings?: Record<string, AttributeListing>; onparameter?: (nodeId: string, parameterId: string, valueType: string, text: string) => void; onreference?: (reference: PortReference, targetPortId: string) => void; onportmenu?: (event: MouseEvent, portId: string, direction: 'input' | 'output') => void; onrequestoptions?: (source: ParameterOptionSource, penSet?: string) => void } = $props()

  /**
   * The stored values PLUS the catalog defaults. A freshly placed node stores
   * nothing, and a control reading only what is stored would show an empty box
   * for a value the evaluator will happily use - and would leave a slider whose
   * bounds live in sibling parameters with no bounds and therefore no track.
   */
  const effective = $derived(withDefaults(definition.parameters, parameters))
  const fields = $derived(fieldsFor(definition.inputs, definition.parameters))
  const sections = $derived(sectionsFor(fields))
  const showHeadings = $derived(shouldShowSectionHeadings(sections))
  const detail = $derived(sections.slice(1))
  const hasDetail = $derived(detail.some((group) => group.fields.length > 0))
  const detailCount = $derived(detail.reduce((total, group) => total + group.fields.length, 0))

  // Collapsed by default and held here rather than in graph state: which
  // sections a user has open is a per-viewer convenience, not something a
  // workflow should carry to whoever opens it next.
  let expanded = $state(false)

  function portFor(portId: string) {
    return definition.inputs.find((input) => input.portId === portId)
  }
  function outputText(portId: string): string | undefined {
    return outputs?.find((output) => output.portId === portId)?.summary
  }
  /** What this port actually produced, so its nub can show item, list or tree. */
  function outputValue(portId: string) {
    return outputs?.find((output) => output.portId === portId)?.value
  }
  function isConnected(portId: string): boolean {
    return connections.some((item) => item.portId === portId && item.direction === 'input' && item.connected)
  }
  /** What a connected input is showing: the upstream value, which is not editable here. */
  function upstreamText(portId: string): string {
    return connections.find((item) => item.portId === portId && item.direction === 'input')?.peerLabels?.[0] ?? 'connected'
  }
  /**
   * A row whose label just repeats the node's own name.
   *
   * A node called Layer with one parameter called Layer said it twice and left
   * the value squeezed into a third of the width - "Interior - Stair & R..." in
   * a box that had room for the whole thing. When the label adds nothing, the
   * control takes its space instead. Compared against the TYPE's label rather
   * than a nickname: renaming a node to "Roof layer" should not suddenly make
   * its parameter label reappear.
   */
  function repeatsNodeName(field: (typeof fields)[number]): boolean {
    return !field.isPort && field.label.trim().toLocaleLowerCase() === definition.label.trim().toLocaleLowerCase()
  }

  /** The right-hand cell: the unit if the descriptor names one, else the type. */
  function suffix(valueType: string, unit: string | undefined): string {
    return unit !== undefined && unit !== '' ? unit : valueType
  }
</script>

{#snippet row(field: (typeof fields)[number])}
  <div
    class="row"
    class:internal={!field.isPort}
    class:wide={field.ui?.widget === 'slider' || field.ui?.widget === 'vector' || field.ui?.widget === 'point'}
    class:unlabelled={repeatsNodeName(field)}
  >
    {#if repeatsNodeName(field)}
      <!-- No label cell at all; the control spans it. -->
    {:else if field.isPort}
      {@const port = portFor(field.id)}
      {#if port !== undefined}
        <NodePort {nodeId} {port} direction="input" {layout} structure={portStructure(port)} connection={connections.find((item) => item.portId === field.id && item.direction === 'input')} messages={messages.filter((message) => message.portId === field.id)} oncontextmenu={onportmenu} />
      {/if}
    {:else}
      <span title={field.ui?.help}>{field.label}</span>
    {/if}
    <ParameterControl
      {field}
      parameters={effective}
      {attributeListings}
      connected={field.isPort && isConnected(field.id)}
      upstream={upstreamText(field.id)}
      disabled={onparameter === undefined}
      oncommit={(id, valueType, text) => onparameter?.(nodeId, id, valueType, text)}
      onreference={onreference}
      {onrequestoptions}
    />
    <small>{suffix(field.valueType, field.ui?.unit)}</small>
  </div>
{/snippet}

<section class="controls nodrag">
  {#each sections[0]?.fields ?? [] as field (field.id)}
    {@render row(field)}
  {/each}

  {#if hasDetail && expanded}
    {#each detail as group (group.section)}
      {#if showHeadings && group.section !== ''}
        <h3>{group.section}</h3>
      {/if}
      {#each group.fields as field (field.id)}
        {@render row(field)}
      {/each}
    {/each}
  {/if}

  {#if hasDetail}
    <button
      type="button"
      class="disclosure"
      aria-expanded={expanded}
      title={expanded ? 'Hide the detailed settings' : `Show ${detailCount} more setting${detailCount === 1 ? '' : 's'}`}
      onclick={() => (expanded = !expanded)}
    >
      <!-- The count is on the collapsed state on purpose: a chevron alone does
           not say whether anything is behind it. -->
      <span class="chevron" class:open={expanded}>⌃</span>
      {#if !expanded}<em>{detailCount}</em>{/if}
    </button>
  {/if}
</section>
<section class="outputs">
  {#each definition.outputs as output}<NodePort {nodeId} port={output} direction="output" {layout} value={outputText(output.portId)} structure={portStructure(output, outputValue(output.portId))} connection={connections.find((item) => item.portId === output.portId && item.direction === 'output')} messages={messages.filter((message) => message.portId === output.portId)} oncontextmenu={onportmenu} />{/each}
</section>

<style>
  /* Zero horizontal padding: the port rows reach the node's edges so the
     handles can sit on them. See NodePort's --port-inset-inline. */
  .controls { padding: 6px 0 0; }
  h3 { margin: 0; padding: 6px 8px 3px 14px; color: var(--text-faint); font: 700 7px/1 ui-monospace, monospace; letter-spacing: .12em; text-transform: uppercase; }
  .row { display: grid; grid-template-columns: minmax(64px, 1fr) minmax(74px, 1.1fr) 30px; align-items: center; min-height: 25px; padding-right: 8px; border-bottom: 1px solid color-mix(in srgb, var(--border) 55%, transparent); }
  /* A slider needs its track AND its field; a point needs three boxes. Both
     take the label's width, because the control is the thing being operated. */
  .row.wide { grid-template-columns: minmax(42px, .55fr) minmax(112px, 2fr) 26px; }
  /* One column for the control, because the label it would have carried is the
     node's own name and is already at the top of the node. */
  .row.unlabelled { grid-template-columns: minmax(0, 1fr) 30px; }
  .row.internal { padding-left: 14px; }
  .row > span { overflow: hidden; color: var(--text-muted); font-size: 9px; text-overflow: ellipsis; white-space: nowrap; }
  small { padding-left: 5px; overflow: hidden; color: var(--text-faint); font: 7px/1 ui-monospace, monospace; text-overflow: ellipsis; }

  .disclosure { display: flex; width: 100%; height: 15px; align-items: center; justify-content: center; padding: 0; border: 0; border-radius: 0; background: transparent; color: var(--text-faint); gap: 5px; cursor: pointer; }
  .disclosure:hover { background: var(--surface-raised); color: var(--text); }
  .disclosure:focus-visible { outline: 1px solid var(--node-color); outline-offset: -1px; }
  .chevron { display: block; font-size: 11px; line-height: 1; transition: transform 120ms; }
  .chevron.open { transform: rotate(180deg); }
  .disclosure em { font: 7px/1 ui-monospace, monospace; font-style: normal; }
  .outputs { display: grid; padding: 3px 0 7px; border-top: 1px solid var(--border); }

  @media (prefers-reduced-motion: reduce) {
    .chevron { transition: none; }
  }
</style>
