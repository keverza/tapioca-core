/**
 * The control layer's PURE half: which control a parameter gets, what it shows,
 * and what it sends back.
 *
 * Everything here is a function of the catalog descriptor and the current
 * values. That is deliberate and is the rule the rest of the control library is
 * built on:
 *
 *   ⚠️ NOTHING IN THIS PACKAGE MAY BRANCH ON `nodeType`. The dispatch key is the
 *   WIDGET, which the runtime owns. A node added to the native catalog has to
 *   render with no change here at all - and the moment one control says "if this
 *   is the slider node", that property is gone and every later node type pays
 *   for it. See HANDOFF-NodeGraphUIBuilder.md sections 4 and 6 (UI-3).
 *
 * It imports nothing at runtime, so the offline `node --test` runner can load it
 * and the policy below is testable without a browser.
 */

import type {
  GraphParameter,
  GraphValue,
  ParameterOption,
  ParameterSchema,
  ParameterUi,
  ParameterWidget,
} from '../../types'

/** The widget set actually rendered. `auto` is resolved away before dispatch. */
export type ControlWidget = Exclude<ParameterWidget, 'auto'>

/**
 * A row to draw, whether it came from a parameter or from an input port.
 *
 * An input port with nothing wired to it takes a typed-in value that the runtime
 * stores as a parameter under the PORT'S OWN ID, so the two are the same thing
 * to a control and are normalised here rather than in the markup.
 */
export interface ControlField {
  id: string
  label: string
  valueType: string
  required: boolean
  ui?: ParameterUi
  /** True when this row also owns a connectable handle. */
  isPort: boolean
}

/**
 * The control a parameter gets.
 *
 * The descriptor wins; the value type is the fallback, which is what every
 * parameter registered before UI-1 relies on. An unknown widget name is treated
 * as no opinion rather than as an error: a client one version behind a runtime
 * that grew a widget should degrade to a sensible box, not to a blank row.
 */
export function resolveWidget(valueType: string, ui?: ParameterUi): ControlWidget {
  const declared = ui?.widget
  if (declared !== undefined && declared !== 'auto' && KNOWN_WIDGETS.has(declared)) {
    return declared as ControlWidget
  }
  if (valueType === 'double' || valueType === 'integer') return 'number'
  if (valueType === 'bool') return 'boolean'
  if (valueType === 'string' || valueType === 'archicadElementRef') return 'text'
  if (valueType === 'point3') return 'vector'
  // list, mesh, polyline, polygon: produced by a node, never authored.
  return 'readOnly'
}

const KNOWN_WIDGETS = new Set<string>([
  'number',
  'slider',
  'boolean',
  'select',
  'text',
  'vector',
  'point',
  'color',
  'readOnly',
])

export interface NumericRange {
  minimum?: number
  maximum?: number
  step?: number
  decimals?: number
}

/**
 * A number control's live bounds.
 *
 * A descriptor may name a SIBLING PARAMETER instead of a constant, and the
 * sibling wins when it holds a usable number - that is what makes a slider whose
 * range the user edits work without this file knowing which node it is on. A
 * named sibling that is empty or unparseable falls back to the constant, so a
 * half-typed range never leaves the slider with no bounds at all.
 */
export function resolveRange(ui: ParameterUi | undefined, parameters: GraphParameter[]): NumericRange {
  if (ui === undefined) return {}
  const fromSibling = (id: string | undefined): number | undefined =>
    id === undefined || id === '' ? undefined : numberOf(parameterValue(parameters, id))
  return {
    minimum: fromSibling(ui.minimumParameter) ?? ui.minimum,
    maximum: fromSibling(ui.maximumParameter) ?? ui.maximum,
    step: fromSibling(ui.stepParameter) ?? ui.step,
    decimals: fromSibling(ui.decimalsParameter) ?? ui.decimals,
  }
}

/**
 * The stored values, with the catalog's defaults filled in for what the node
 * has not stored.
 *
 * ⚠️ A FRESHLY PLACED NODE STORES NOTHING. The runtime applies a parameter's
 * `defaultValue` when it evaluates, so a control reading only the stored
 * parameters shows an empty box for a value the graph really has - and, worse,
 * a slider whose range lives in sibling parameters finds no bounds at all and
 * draws no track. Merging the defaults in HERE, once, is what makes every
 * control show the same value the evaluator would use.
 *
 * Stored always wins; a default never overwrites an authored value.
 */
export function withDefaults(schema: ParameterSchema[], stored: GraphParameter[]): GraphParameter[] {
  const merged = stored.slice()
  for (const parameter of schema) {
    if (parameter.defaultValue === undefined) continue
    if (merged.some((item) => item.parameterId === parameter.parameterId)) continue
    merged.push({ parameterId: parameter.parameterId, value: parameter.defaultValue })
  }
  return merged
}

/** Whether a slider can be drawn at all: a slider without both ends cannot. */
export function isDraggable(range: NumericRange): boolean {
  return (
    range.minimum !== undefined &&
    range.maximum !== undefined &&
    Number.isFinite(range.minimum) &&
    Number.isFinite(range.maximum) &&
    range.maximum > range.minimum
  )
}

/**
 * The step a number input advances by.
 *
 * Derived from the decimal places when no step is declared, because those two
 * disagreeing is the arrangement that produces a field the user cannot type a
 * legal value into: a step of 1 on a two-decimal value rejects 1.25 in a browser
 * that validates the step.
 */
export function stepFor(range: NumericRange): number {
  if (range.step !== undefined && range.step > 0) return range.step
  if (range.decimals !== undefined && range.decimals >= 0) return 10 ** -range.decimals
  return 0.001
}

export function clampNumber(value: number, range: NumericRange): number {
  let clamped = value
  if (range.minimum !== undefined && Number.isFinite(range.minimum)) clamped = Math.max(clamped, range.minimum)
  if (range.maximum !== undefined && Number.isFinite(range.maximum)) clamped = Math.min(clamped, range.maximum)
  return clamped
}

/**
 * A number as the user sees it.
 *
 * `toFixed`, not a locale format: this string is also what gets COMMITTED, and a
 * locale that writes 1,25 would submit a value the runtime reads as two numbers.
 * The display and the wire spelling must be the same string.
 */
export function formatNumber(value: number | undefined, decimals?: number): string {
  if (value === undefined || !Number.isFinite(value)) return ''
  if (decimals === undefined || decimals < 0) return String(value)
  return value.toFixed(Math.min(Math.trunc(decimals), 15))
}

/** The three components of a point3 value, missing ones read as zero. */
export function componentsOf(value: GraphValue | undefined, count = 3): number[] {
  const numbers = value?.numbers ?? []
  return Array.from({ length: count }, (_, index) => {
    const component = numbers[index]
    return typeof component === 'number' && Number.isFinite(component) ? component : 0
  })
}

/**
 * Components back into the text `graphValueFromText` parses for point3.
 *
 * Committing through the same text path every other control uses keeps ONE
 * conversion in the editor rather than a second encoder here.
 */
export function formatComponents(components: number[], decimals?: number): string {
  return components.map((component) => formatNumber(component, decimals) || '0').join(', ')
}

/** The axis labels for a point3 row; the descriptor may rename them. */
export function componentLabels(ui: ParameterUi | undefined, count = 3): string[] {
  const declared = ui?.components ?? []
  const fallback = ['X', 'Y', 'Z', 'W']
  return Array.from({ length: count }, (_, index) => declared[index] ?? fallback[index] ?? String(index + 1))
}

/**
 * A value's identity for option matching.
 *
 * Compared as a STRING because that is what an HTML select carries, and because
 * the same choice can arrive as `1` from the runtime and `"1"` from the DOM.
 * Only ever used to decide which option is selected - never to submit anything.
 */
export function optionKey(value: GraphValue | undefined): string {
  if (value === undefined) return ''
  if (value.text !== undefined) return value.text
  if (value.number !== undefined) return String(value.number)
  if (value.bool !== undefined) return value.bool ? 'true' : 'false'
  if (value.numbers !== undefined) return value.numbers.join(',')
  return ''
}

/**
 * Whether the current value is one of the offered options.
 *
 * A graph can carry a layer name the open project does not have - a workflow
 * saved against another project, or an attribute somebody deleted. That must
 * read as a MISSING CHOICE the user can see, never as a silent snap to the first
 * option, which would rewrite the graph without being asked.
 */
export function isKnownOption(options: ParameterOption[], value: GraphValue | undefined): boolean {
  const key = optionKey(value)
  if (key === '') return true
  return options.some((option) => optionKey(option.value) === key)
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
}

/**
 * The native listing, as options.
 *
 * `name` or `number` is what gets stored, and which one it is comes from the
 * PARAMETER'S value type rather than from the row - a pen picker declares an
 * integer parameter and every other picker declares a string, so the type is
 * already the answer and no kind-specific branch is needed here.
 */
export function optionsFromAttributes(rows: AttributeRow[], valueType: string): ParameterOption[] {
  const options: ParameterOption[] = []
  for (const row of rows) {
    if (valueType === 'integer') {
      if (typeof row.number !== 'number') continue
      options.push({ label: row.label, value: { valueType, number: row.number } })
      continue
    }
    if (row.name === undefined || row.name === '') continue
    options.push({ label: row.label, value: { valueType: 'string', text: row.name } })
  }
  return options
}

/** The stored value of one parameter. */
export function parameterValue(parameters: GraphParameter[], id: string): GraphValue | undefined {
  const stored = parameters.find((parameter) => parameter.parameterId === id)
  if (stored?.value !== undefined) return stored.value
  // The deprecated alias, still accepted so a graph written by an older client
  // renders rather than showing an empty box.
  if (stored?.numberValue !== undefined) return { valueType: 'double', number: stored.numberValue }
  return undefined
}

export function numberOf(value: GraphValue | undefined): number | undefined {
  return typeof value?.number === 'number' && Number.isFinite(value.number) ? value.number : undefined
}

export function boolOf(value: GraphValue | undefined): boolean {
  return value?.bool === true
}

export function textOf(value: GraphValue | undefined): string {
  if (value?.text !== undefined) return value.text
  if (value?.number !== undefined) return String(value.number)
  if (value?.bool !== undefined) return value.bool ? 'True' : 'False'
  if (value?.numbers !== undefined) return value.numbers.join(', ')
  return ''
}

/** What a control sends for a boolean, in the spelling `graphValueFromText` reads. */
export function booleanText(value: boolean): string {
  return value ? 'true' : 'false'
}

/**
 * The rows of one node body, in the order and grouping the runtime asked for.
 *
 * Inputs come first and keep their declared order - they carry the handles, and
 * a port that moved between renders is a wire that moved. Parameters that are
 * not also inputs follow, sorted by the descriptor's `order` with the array
 * position as the tiebreak, so a catalog that declares no order is unchanged.
 */
export function fieldsFor(
  inputs: { portId: string; label: string; valueType: string; required?: boolean }[],
  parameters: ParameterSchema[],
): ControlField[] {
  const portIds = new Set(inputs.map((input) => input.portId))
  const byId = new Map(parameters.map((parameter) => [parameter.parameterId, parameter]))
  const fields: ControlField[] = inputs.map((input) => ({
    id: input.portId,
    label: input.label,
    valueType: input.valueType,
    required: input.required === true,
    ui: byId.get(input.portId)?.ui,
    isPort: true,
  }))
  const internal = parameters
    .map((parameter, index) => ({ parameter, index }))
    .filter(({ parameter }) => !portIds.has(parameter.parameterId))
    .sort((left, right) => {
      const order = (left.parameter.ui?.order ?? 0) - (right.parameter.ui?.order ?? 0)
      return order !== 0 ? order : left.index - right.index
    })
  for (const { parameter } of internal) {
    fields.push({
      id: parameter.parameterId,
      label: parameter.label,
      valueType: parameter.valueType,
      required: parameter.required,
      ui: parameter.ui,
      isPort: false,
    })
  }
  return fields
}

/**
 * The fields grouped under their section headings.
 *
 * A heading is only worth drawing when there is more than one group: a node
 * whose every parameter says "Value" would otherwise get a header that
 * distinguishes nothing, on a body where vertical space is the scarce thing.
 */
export function sectionsFor(fields: ControlField[]): { section: string; fields: ControlField[] }[] {
  const sections: { section: string; fields: ControlField[] }[] = []
  for (const field of fields) {
    const section = field.ui?.section ?? ''
    const existing = sections.find((candidate) => candidate.section === section)
    if (existing === undefined) sections.push({ section, fields: [field] })
    else existing.fields.push(field)
  }
  return sections
}

export function shouldShowSectionHeadings(sections: { section: string }[]): boolean {
  return sections.filter((section) => section.section !== '').length > 1
}
