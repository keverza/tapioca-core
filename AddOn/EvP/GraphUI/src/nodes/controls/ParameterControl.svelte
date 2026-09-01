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
  import type { AttributeListing, GraphParameter, GraphValue, ParameterOptionSource } from '../../types'
  import { parsePortReference, type PortReference } from '../types/portReference'
  import AttributePicker from './AttributePicker.svelte'
  import BooleanControl from './BooleanControl.svelte'
  import NumberControl from './NumberControl.svelte'
  import PreviewTargetControl from './PreviewTargetControl.svelte'
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
    attributeListings = {},
    disabled = false,
    oncommit,
    onreference,
    onrequestoptions,
  }: {
    field: ControlField
    parameters: GraphParameter[]
    connected?: boolean
    upstream?: string
    attributeListings?: Record<string, AttributeListing>
    disabled?: boolean
    oncommit: (id: string, valueType: string, text: string) => void
    onreference?: (reference: PortReference, portId: string) => void
    onrequestoptions?: (source: ParameterOptionSource, penSet?: string) => void
  } = $props()

  const widget = $derived(resolveWidget(field.valueType, field.ui))
  const value = $derived<GraphValue | undefined>(parameterValue(parameters, field.id))
  const range = $derived(resolveRange(field.ui, parameters))
  const source = $derived(field.ui?.optionSource ?? 'none')
  // Literal options declared by the node type. An Archicad domain has none and
  // is served by the picker below instead.
  const options = $derived(field.ui?.options ?? [])
  const listing = $derived(attributeListings[source])
  const choices = $derived(choicesFromAttributes(listing?.attributes ?? [], field.valueType))
  // A source that has been asked for and has not answered yet. `undefined` is
  // "never asked"; an empty listing is "asked, and the project has none".
  const loading = $derived(source !== 'none' && listing === undefined)

  function commit(text: string): void {
    oncommit(field.id, field.valueType, text)
  }

  function handlePaste(event: ClipboardEvent): void {
    if (!field.isPort || onreference === undefined) return
    const reference = parsePortReference(event.clipboardData?.getData('text/plain') ?? '')
    if (reference === undefined) return
    event.preventDefault()
    event.stopPropagation()
    onreference(reference, field.id)
  }
</script>

<div class="control" onpaste={handlePaste}>
{#if connected}
  <!-- Focusable because it is a PASTE TARGET - a port reference dropped on a
       connected input is a rewire request, and a paste needs somewhere to land.
       A readonly <input> rather than an <output> carrying a tabindex: <output>
       is non-interactive by definition, so making it tabbable is a claim no role
       can honestly back, while a readonly input IS the element for "text you can
       focus and paste over but not type into". -->
  <input class="upstream" title={upstream} value={upstream} readonly aria-label={`${field.label}, from ${upstream}`} />
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
{:else if widget === 'previewTarget'}
  <PreviewTargetControl {value} options={field.ui?.options ?? []} label={field.label} {disabled} oncommit={commit} />
{:else if widget === 'select' && source !== 'none'}
  <!-- An Archicad domain. A native <option> can carry text and nothing else, so
       a fill list would read as "25 %, 50 %, 75 %" and a composite list as a
       wall of near-identical names; the picker draws the swatch, the folder and
       the search box that make those lists usable. -->
  <AttributePicker
    {value}
    {choices}
    {listing}
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
  />
{:else}
  <!-- readOnly: a list, a mesh, a polyline. Produced by a node, never authored,
       so the honest control is the value itself. -->
  <output class="read-only" title={textOf(value)}>{textOf(value) || '—'}</output>
{/if}
</div>

<style>
  .control { min-width: 0; }
  output, .upstream { display: block; box-sizing: border-box; width: 100%; overflow: hidden; min-height: 19px; padding: 4px 5px; border: 0; background: var(--canvas); color: var(--text-faint); font: 9px/1.2 ui-monospace, monospace; text-overflow: ellipsis; white-space: nowrap; }
  /* Dashed, because a connected input is showing a value it does not own. */
  .upstream { border: 1px dashed #858b94; color: #b5bac2; }
  .upstream:focus { border-color: var(--node-color); outline: none; }
  .read-only { font-style: italic; }
</style>
