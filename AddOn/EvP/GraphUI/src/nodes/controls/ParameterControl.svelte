<script lang="ts">
  /**
   * The one place a widget name becomes a component.
   *
   * ⚠️ THE DISPATCH KEY IS THE WIDGET AND NOTHING ELSE. No branch here or below
   * may look at `nodeType`: a node type added to the native catalog has to
   * render with no change in this package, which is the property UI-1 through
   * UI-3 exist to buy. The widget vocabulary is closed and validated by the
   * runtime at registration, so an unknown one cannot arrive from a graph file -
   * only from a client older than its runtime, which `resolveWidget` degrades to
   * a typed box rather than to a blank row.
   *
   * A CONNECTED input is not editable, and shows the upstream value instead: the
   * wire is the value, and a box that still took typing would be offering an
   * edit the runtime discards.
   */
  import type { AttributeRow, GraphParameter, GraphValue, ParameterOptionSource } from '../../types'
  import type { PortReference } from '../types/portReference'
  import AttributePicker from './AttributePicker.svelte'
  import BooleanControl from './BooleanControl.svelte'
  import NumberControl from './NumberControl.svelte'
  import SelectControl from './SelectControl.svelte'
  import TextControl from './TextControl.svelte'
  import VectorControl from './VectorControl.svelte'
  import {
    boolOf,
    choicesFromAttributes,
    numberOf,
    parameterValue,
    resolveRange,
    resolveWidget,
    textOf,
    type ControlField,
  } from './widgets'

  let {
    field,
    parameters,
    connected = false,
    upstream = '',
    attributeRows = {},
    disabled = false,
    oncommit,
    onreference,
    onrequestoptions,
  }: {
    field: ControlField
    parameters: GraphParameter[]
    connected?: boolean
    upstream?: string
    attributeRows?: Record<string, AttributeRow[]>
    disabled?: boolean
    oncommit: (id: string, valueType: string, text: string) => void
    onreference?: (reference: PortReference, portId: string) => void
    onrequestoptions?: (source: ParameterOptionSource) => void
  } = $props()

  const widget = $derived(resolveWidget(field.valueType, field.ui))
  const value = $derived<GraphValue | undefined>(parameterValue(parameters, field.id))
  const range = $derived(resolveRange(field.ui, parameters))
  const source = $derived(field.ui?.optionSource ?? 'none')
  // Literal options declared by the node type. An Archicad domain has none and
  // is served by the picker below instead.
  const options = $derived(field.ui?.options ?? [])
  const choices = $derived(choicesFromAttributes(attributeRows[source] ?? [], field.valueType))
  // A source that has been asked for and has not answered yet. `undefined` is
  // "never asked"; an empty array is "asked, and the project has none".
  const loading = $derived(source !== 'none' && attributeRows[source] === undefined)

  function commit(text: string): void {
    oncommit(field.id, field.valueType, text)
  }
</script>

{#if connected}
  <output class="upstream" title={upstream}>{upstream}</output>
{:else if widget === 'number' || widget === 'slider'}
  <NumberControl
    value={numberOf(value)}
    {range}
    label={field.label}
    {disabled}
    slider={widget === 'slider'}
    oncommit={commit}
  />
{:else if widget === 'boolean'}
  <BooleanControl value={boolOf(value)} label={field.label} {disabled} oncommit={commit} />
{:else if widget === 'select' && source !== 'none'}
  <!-- An Archicad domain. A native <option> can carry text and nothing else, so
       a fill list would read as "25 %, 50 %, 75 %" and a composite list as a
       wall of near-identical names; the picker draws the swatch, the folder and
       the search box that make those lists usable. -->
  <AttributePicker
    {value}
    {choices}
    {source}
    {loading}
    label={field.label}
    {disabled}
    oncommit={commit}
    onrequest={onrequestoptions}
  />
{:else if widget === 'select'}
  <SelectControl
    {value}
    {options}
    {source}
    {loading}
    label={field.label}
    {disabled}
    oncommit={commit}
    onrequest={onrequestoptions}
  />
{:else if widget === 'vector' || widget === 'point'}
  <VectorControl {value} ui={field.ui} label={field.label} {disabled} oncommit={commit} />
{:else if widget === 'text' || widget === 'color'}
  <TextControl
    value={textOf(value)}
    label={field.label}
    placeholder={field.valueType}
    swatch={widget === 'color'}
    {disabled}
    oncommit={commit}
    onreference={onreference === undefined ? undefined : (reference) => onreference(reference, field.id)}
  />
{:else}
  <!-- readOnly: a list, a mesh, a polyline. Produced by a node, never authored,
       so the honest control is the value itself. -->
  <output class="read-only" title={textOf(value)}>{textOf(value) || '—'}</output>
{/if}

<style>
  output { display: block; overflow: hidden; min-height: 19px; padding: 4px 5px; background: var(--canvas); color: var(--text-faint); font: 9px/1.2 ui-monospace, monospace; text-overflow: ellipsis; white-space: nowrap; }
  .read-only { font-style: italic; }
</style>
