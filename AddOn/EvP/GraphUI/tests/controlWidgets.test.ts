import assert from 'node:assert/strict'
import test from 'node:test'
import {
  boolOf,
  clampNumber,
  componentLabels,
  componentsOf,
  fieldsFor,
  formatComponents,
  formatNumber,
  isDraggable,
  isKnownOption,
  numberOf,
  optionKey,
  optionsFromAttributes,
  parameterValue,
  resolveRange,
  resolveWidget,
  sectionsFor,
  shouldShowSectionHeadings,
  stepFor,
  textOf,
  withDefaults,
} from '../src/nodes/controls/widgets.ts'
import {
  ANY_PORT_COLOR,
  describeStructure,
  portColor,
  portStructure,
  structureOfValue,
} from '../src/nodes/types/display.ts'
import type { GraphParameter, ParameterSchema, ParameterUi } from '../src/types.ts'

function ui(overrides: Partial<ParameterUi> = {}): ParameterUi {
  return {
    widget: 'auto',
    section: '',
    order: 0,
    help: '',
    unit: '',
    components: [],
    options: [],
    optionSource: 'none',
    ...overrides,
  }
}

test('a parameter with no descriptor still gets a control from its value type', () => {
  // Every parameter registered before the UI descriptor existed is in this
  // state; the fallback is what keeps them editable.
  assert.equal(resolveWidget('double'), 'number')
  assert.equal(resolveWidget('integer'), 'number')
  assert.equal(resolveWidget('bool'), 'boolean')
  assert.equal(resolveWidget('string'), 'text')
  assert.equal(resolveWidget('point3'), 'vector')
  assert.equal(resolveWidget('list'), 'readOnly')
  assert.equal(resolveWidget('mesh'), 'readOnly')
})

test('the descriptor wins over the value type, and an unknown widget degrades', () => {
  assert.equal(resolveWidget('double', ui({ widget: 'slider' })), 'slider')
  assert.equal(resolveWidget('string', ui({ widget: 'select' })), 'select')
  assert.equal(resolveWidget('double', ui({ widget: 'auto' })), 'number')
  // A client one version behind a runtime that grew a widget must fall back to
  // a usable box rather than render nothing.
  assert.equal(resolveWidget('double', ui({ widget: 'curveEditor' as never })), 'number')
})

test('a bound naming a sibling parameter beats the constant, and an empty sibling does not', () => {
  const descriptor = ui({
    widget: 'slider',
    minimum: 0,
    maximum: 100,
    decimals: 2,
    minimumParameter: 'minimum',
    maximumParameter: 'maximum',
    decimalsParameter: 'decimals',
  })
  const parameters: GraphParameter[] = [
    { parameterId: 'minimum', value: { valueType: 'double', number: -5 } },
    { parameterId: 'maximum', value: { valueType: 'double', number: 5 } },
    { parameterId: 'decimals', value: { valueType: 'integer', number: 3 } },
  ]
  assert.deepEqual(resolveRange(descriptor, parameters), {
    minimum: -5,
    maximum: 5,
    step: undefined,
    decimals: 3,
  })

  // A half-typed range must not leave the slider with no bounds at all.
  assert.deepEqual(resolveRange(descriptor, []), { minimum: 0, maximum: 100, step: undefined, decimals: 2 })
})

test('a slider is only draggable when it has two distinct ends', () => {
  assert.equal(isDraggable({ minimum: 0, maximum: 10 }), true)
  assert.equal(isDraggable({ minimum: 0 }), false)
  assert.equal(isDraggable({ minimum: 5, maximum: 5 }), false)
  assert.equal(isDraggable({ minimum: 10, maximum: 0 }), false)
  assert.equal(isDraggable({}), false)
})

test('the step follows the decimal places when none is declared', () => {
  // A step of 1 on a two-decimal value rejects 1.25 in a browser that validates
  // the step, which makes the field impossible to type a legal value into.
  assert.equal(stepFor({ decimals: 2 }), 0.01)
  assert.equal(stepFor({ decimals: 0 }), 1)
  assert.equal(stepFor({ decimals: 2, step: 0.5 }), 0.5)
  assert.equal(stepFor({}), 0.001)
})

test('a number is formatted the way it is committed', () => {
  // The displayed string is also the submitted one, so it must never be
  // locale-formatted: "1,25" would reach the runtime as two numbers.
  assert.equal(formatNumber(1.23456, 2), '1.23')
  assert.equal(formatNumber(1, 0), '1')
  assert.equal(formatNumber(1.5, undefined), '1.5')
  assert.equal(formatNumber(undefined, 2), '')
  assert.equal(formatNumber(Number.NaN, 2), '')
  assert.equal(clampNumber(999, { minimum: 0, maximum: 10 }), 10)
  assert.equal(clampNumber(-1, { minimum: 0, maximum: 10 }), 0)
  assert.equal(clampNumber(-1, {}), -1)
})

test('a point3 renders as components and commits as the text the editor parses', () => {
  const value = { valueType: 'point3', numbers: [1, 2, 3] }
  assert.deepEqual(componentsOf(value), [1, 2, 3])
  // A short or absent value reads as zeroes rather than as blank fields, so the
  // row never shows fewer boxes than the type has components.
  assert.deepEqual(componentsOf({ valueType: 'point3', numbers: [1] }), [1, 0, 0])
  assert.deepEqual(componentsOf(undefined), [0, 0, 0])
  assert.equal(formatComponents([1, 2, 3], 2), '1.00, 2.00, 3.00')
  assert.deepEqual(componentLabels(ui({ components: ['U', 'V', 'W'] })), ['U', 'V', 'W'])
  assert.deepEqual(componentLabels(undefined), ['X', 'Y', 'Z'])
})

test('an option is identified by its value, whatever type carried it', () => {
  assert.equal(optionKey({ valueType: 'string', text: 'Concrete' }), 'Concrete')
  assert.equal(optionKey({ valueType: 'integer', number: 3 }), '3')
  assert.equal(optionKey(undefined), '')
})

test('a value the project no longer has stays selected instead of snapping', () => {
  // A workflow saved against another project can name a layer this one lacks.
  // Correcting it silently would rewrite the document without being asked.
  const options = [{ label: 'Existing', value: { valueType: 'string', text: 'Existing' } }]
  assert.equal(isKnownOption(options, { valueType: 'string', text: 'Existing' }), true)
  assert.equal(isKnownOption(options, { valueType: 'string', text: 'Deleted Layer' }), false)
  // Nothing chosen yet is not a missing choice.
  assert.equal(isKnownOption(options, undefined), true)
})

test('the attribute listing becomes options keyed by name, or by number for pens', () => {
  const rows = [
    { label: 'Concrete', name: 'Concrete', index: 1 },
    { label: 'No name', index: 2 },
  ]
  assert.deepEqual(optionsFromAttributes(rows, 'string'), [
    { label: 'Concrete', value: { valueType: 'string', text: 'Concrete' } },
  ])

  const pens = [
    { label: '1  Thin', number: 1, index: 1, color: '#101010' },
    { label: 'Nameless', index: 2 },
  ]
  assert.deepEqual(optionsFromAttributes(pens, 'integer'), [
    { label: '1  Thin', value: { valueType: 'integer', number: 1 } },
  ])
})

test('a stored value is read through the current encoding and the deprecated alias', () => {
  const parameters: GraphParameter[] = [
    { parameterId: 'a', value: { valueType: 'double', number: 2 } },
    { parameterId: 'b', numberValue: 7 },
  ]
  assert.equal(numberOf(parameterValue(parameters, 'a')), 2)
  assert.equal(numberOf(parameterValue(parameters, 'b')), 7)
  assert.equal(parameterValue(parameters, 'missing'), undefined)
  assert.equal(boolOf({ valueType: 'bool', bool: true }), true)
  assert.equal(boolOf(undefined), false)
  assert.equal(textOf({ valueType: 'point3', numbers: [1, 2, 3] }), '1, 2, 3')
})

test('inputs keep their declared order and parameters follow it in descriptor order', () => {
  // The inputs carry the handles, so a port that moved between renders is a
  // wire that moved - they are never reordered by the descriptor.
  const inputs = [
    { portId: 'point', label: 'Point', valueType: 'point3' },
    { portId: 'scale', label: 'Scale', valueType: 'double' },
  ]
  const parameters: ParameterSchema[] = [
    { parameterId: 'point', label: 'Point', valueType: 'point3', required: false, ui: ui({ widget: 'point' }) },
    { parameterId: 'later', label: 'Later', valueType: 'double', required: false, ui: ui({ order: 9 }) },
    { parameterId: 'earlier', label: 'Earlier', valueType: 'double', required: false, ui: ui({ order: 1 }) },
  ]
  const fields = fieldsFor(inputs, parameters)
  assert.deepEqual(fields.map((field) => field.id), ['point', 'scale', 'earlier', 'later'])
  assert.deepEqual(fields.map((field) => field.isPort), [true, true, false, false])
  // The port row picks up the matching parameter's descriptor, which is how an
  // internalised input gets the same control as the parameter behind it.
  assert.equal(fields[0]?.ui?.widget, 'point')
  // An input with no matching parameter simply has none.
  assert.equal(fields[1]?.ui, undefined)
})

test('section headings are drawn only when they distinguish something', () => {
  const parameters: ParameterSchema[] = [
    { parameterId: 'value', label: 'Value', valueType: 'double', required: false, ui: ui({ section: 'Value' }) },
    { parameterId: 'minimum', label: 'Minimum', valueType: 'double', required: false, ui: ui({ section: 'Range', order: 1 }) },
    { parameterId: 'maximum', label: 'Maximum', valueType: 'double', required: false, ui: ui({ section: 'Range', order: 2 }) },
  ]
  const sections = sectionsFor(fieldsFor([], parameters))
  assert.deepEqual(sections.map((section) => section.section), ['Value', 'Range'])
  assert.deepEqual(sections[1]?.fields.map((field) => field.id), ['minimum', 'maximum'])
  assert.equal(shouldShowSectionHeadings(sections), true)

  const single = sectionsFor(fieldsFor([], [parameters[0] as ParameterSchema]))
  assert.equal(shouldShowSectionHeadings(single), false)
})


test('a freshly placed node still shows the values the evaluator would use', () => {
  // The regression this exists for: a node stores no parameters until something
  // is edited, so a slider whose bounds live in sibling parameters found none
  // and drew no track at all.
  const schema: ParameterSchema[] = [
    { parameterId: 'value', label: 'Value', valueType: 'double', required: true, defaultValue: { valueType: 'double', number: 0 } },
    { parameterId: 'minimum', label: 'Minimum', valueType: 'double', required: false, defaultValue: { valueType: 'double', number: 0 } },
    { parameterId: 'maximum', label: 'Maximum', valueType: 'double', required: false, defaultValue: { valueType: 'double', number: 100 } },
    { parameterId: 'nodefault', label: 'No default', valueType: 'double', required: false },
  ]
  const merged = withDefaults(schema, [])
  assert.deepEqual(merged.map((item) => item.parameterId), ['value', 'minimum', 'maximum'])

  const descriptor = ui({ widget: 'slider', minimumParameter: 'minimum', maximumParameter: 'maximum' })
  assert.equal(isDraggable(resolveRange(descriptor, [])), false)
  assert.equal(isDraggable(resolveRange(descriptor, merged)), true)

  // An authored value always beats the default.
  const stored = [{ parameterId: 'maximum', value: { valueType: 'double', number: 5 } }]
  assert.equal(resolveRange(descriptor, withDefaults(schema, stored)).maximum, 5)
})

test('a step may come from a sibling parameter like the range does', () => {
  const descriptor = ui({ widget: 'slider', minimum: 0, maximum: 10, step: 1, stepParameter: 'step' })
  const parameters: GraphParameter[] = [{ parameterId: 'step', value: { valueType: 'double', number: 0.25 } }]
  assert.equal(resolveRange(descriptor, parameters).step, 0.25)
  assert.equal(stepFor(resolveRange(descriptor, parameters)), 0.25)
  assert.equal(resolveRange(descriptor, []).step, 1)
})

test('a port nub is coloured by its value type, and geometry shares one hue', () => {
  // Before a wire is dragged the only question is "will these two connect", so
  // the colour answers the TYPE, not which node the port belongs to.
  assert.equal(portColor('double'), portColor('integer'))
  assert.equal(portColor('point3'), portColor('mesh'))
  assert.equal(portColor('polyline'), portColor('polygon'))
  assert.notEqual(portColor('double'), portColor('bool'))
  assert.notEqual(portColor('string'), portColor('double'))
  assert.notEqual(portColor('list'), portColor('mesh'))
  // `absent` means "any type" in the runtime, and an unknown one must still draw.
  assert.equal(portColor('absent'), ANY_PORT_COLOR)
  assert.equal(portColor('somethingNew'), ANY_PORT_COLOR)
})

test('a port shows item, list or tree before anything is ever wired', () => {
  // The point of drawing it unconnected: a structure mismatch is caught before
  // the drag, not after the answer looks odd.
  assert.equal(portStructure({ valueType: 'double' }), 'item')
  assert.equal(portStructure({ valueType: 'list' }), 'list')
  // An input that accepts several connections is a list input, whatever one
  // wire would carry.
  assert.equal(portStructure({ valueType: 'double', acceptsMultiple: true }), 'list')
})

test('a produced value overrides the declared structure, and nesting reads as a tree', () => {
  const port = { valueType: 'list' }
  assert.equal(portStructure(port, { valueType: 'list', items: [{ valueType: 'double' }] }), 'list')
  assert.equal(portStructure(port, { valueType: 'list', items: [{ valueType: 'list' }] }), 'tree')
  // A node declared to emit a list that produced ONE thing says so, which is the
  // case a declared-only hint would get wrong.
  assert.equal(portStructure(port, { valueType: 'double', number: 1 }), 'item')
  assert.equal(structureOfValue(undefined), undefined)
  assert.equal(structureOfValue({ valueType: 'list' }), 'list')
})

test('every structure has words as well as a shape', () => {
  // Shape and colour are never the only channel; the hover card says it too.
  assert.equal(describeStructure('item'), 'single item')
  assert.equal(describeStructure('list'), 'list')
  assert.equal(describeStructure('tree'), 'tree of lists')
})
