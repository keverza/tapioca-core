import assert from 'node:assert/strict'
import test from 'node:test'
import {
  containerElementType,
  containerGroupOf,
  elementGroupsOf,
  settingSectionsOf,
  stackedCount,
  unreadSettingCount,
  UNCLASSIFIED_ELEMENT_TYPE,
  type ElementTypeInfo,
  propertyListOf,
  elementDisplayName,
} from '../src/nodes/archicad/elements.ts'
import type {
  ElementDescription,
  ElementSettingSchema,
  ElementTypeSchema,
  GraphParameter,
} from '../src/types.ts'

/**
 * ⚠️ WHAT THESE TESTS PROTECT. Two lists have to stay PARALLEL - the guids a
 * selection captured and the element types captured with them - and the failure
 * when they slip is invisible: a stack that adds up to the right number and is
 * wrong about every element in it. The rest is about never rendering a label the
 * runtime did not send, and never drawing an unread setting as a blank one.
 */

const catalog: ElementTypeInfo[] = [
  { id: 'wall', label: 'Wall', plural: 'Walls', container: true },
  { id: 'slab', label: 'Slab', plural: 'Slabs', container: true },
  { id: 'column', label: 'Column', plural: 'Columns', container: true },
  { id: 'text', label: 'Text', plural: 'Texts', container: false },
  { id: UNCLASSIFIED_ELEMENT_TYPE, label: 'Other', plural: 'Other Elements', container: false },
]

function captured(guids: string[], types: string[]): GraphParameter[] {
  return [
    { parameterId: 'elements', value: { valueType: 'list', items: guids.map((text) => ({ valueType: 'archicadElementRef', text })) } },
    { parameterId: 'elementTypes', value: { valueType: 'list', items: types.map((text) => ({ valueType: 'string', text })) } },
  ]
}

test('the stack follows the runtime catalog order, not the selection order', () => {
  // The same model, clicked in a different order, has to draw the same panel.
  const groups = elementGroupsOf(captured(['c', 's', 'w', 'w2'], ['column', 'slab', 'wall', 'wall']), catalog)

  assert.deepEqual(
    groups.map((group) => group.elementType),
    ['wall', 'slab', 'column'],
  )
  assert.equal(groups[0].label, 'Walls')
  // Order INSIDE a group is Archicad's own selection order, which is the one the
  // user can recognise.
  assert.deepEqual(groups[0].guids, ['w', 'w2'])
  assert.equal(stackedCount(groups), 4)
})

test('a type list shorter than its guids leaves the remainder unclassified, never shifted', () => {
  // ⚠️ THE FAILURE THIS EXISTS FOR. Dropping the extras would make a selection of
  // three show a stack of one; filing them under the last known type would give
  // a stack that adds up and lies about every element in it.
  const groups = elementGroupsOf(captured(['a', 'b', 'c'], ['wall']), catalog)

  assert.deepEqual(
    groups.map((group) => group.elementType),
    ['wall', UNCLASSIFIED_ELEMENT_TYPE],
  )
  assert.deepEqual(groups[0].guids, ['a'])
  assert.deepEqual(groups[1].guids, ['b', 'c'])
  assert.equal(stackedCount(groups), 3)
})

test('a type the catalog does not name is bucketed rather than dropped', () => {
  const groups = elementGroupsOf(captured(['a', 'b'], ['wall', 'notAType']), catalog)
  assert.equal(groups.length, 2)
  assert.equal(groups[1].elementType, UNCLASSIFIED_ELEMENT_TYPE)
})

test('a node with no capture stacks nothing', () => {
  assert.deepEqual(elementGroupsOf([], catalog), [])
  assert.deepEqual(elementGroupsOf(captured([], []), catalog), [])
  // ... and an empty catalog stacks nothing rather than inventing an order.
  assert.deepEqual(elementGroupsOf(captured(['a'], ['wall']), []), [])
})

// --- the settings tree ------------------------------------------------------

/**
 * One descriptor, with the fields a test does not care about defaulted. The
 * runtime always sends all of them; spelling them out on every row would make
 * these fixtures unreadable and would bury the one field a given test is about.
 */
function setting(
  id: string,
  label: string,
  group: string,
  valueType: string,
  unit = '',
  extra: Partial<ElementSettingSchema> = {},
): ElementSettingSchema {
  return {
    id,
    label,
    group,
    valueType,
    unit,
    origin: 'archicad',
    appliesWhenSetting: '',
    appliesWhenEquals: '',
    ...extra,
  }
}

const wallSchema: ElementTypeSchema = {
  id: 'wall',
  label: 'Wall',
  plural: 'Walls',
  settings: [
    setting('elementId', 'ID', 'Identity', 'string'),
    setting('layer', 'Layer', 'Identity', 'string'),
    setting('referenceLine', 'Reference Line Location', 'Placement', 'string'),
    setting('thickness', 'Thickness', 'Geometry', 'double', 'm'),
    setting('height', 'Height', 'Geometry', 'double', 'm'),
  ],
}

/**
 * A wall whose three material rows are conditional on its structure, which is
 * how the runtime actually declares them.
 */
const structuredWallSchema: ElementTypeSchema = {
  id: 'wall',
  label: 'Wall',
  plural: 'Walls',
  settings: [
    setting('elementId', 'ID', 'Identity', 'string'),
    setting('structure', 'Structure', 'Structure', 'string'),
    setting('buildingMaterial', 'Building Material', 'Structure', 'string', '', {
      appliesWhenSetting: 'structure',
      appliesWhenEquals: 'Basic',
    }),
    setting('composite', 'Composite', 'Structure', 'string', '', {
      appliesWhenSetting: 'structure',
      appliesWhenEquals: 'Composite',
    }),
    setting('length', 'Length', 'Geometry', 'double', 'm', { origin: 'derived' }),
  ],
}

function described(settings: { id: string; text: string }[]): ElementDescription {
  return {
    guid: 'w',
    elementType: 'wall',
    typeLabel: 'Wall',
    available: true,
    detail: '',
    settings: settings.map((setting) => ({ ...setting, hasNumber: false, number: 0 })),
  }
}

test('settings group in the runtime order, and a group appears once', () => {
  const sections = settingSectionsOf(
    described([
      { id: 'thickness', text: '0.3' },
      { id: 'elementId', text: 'W-01' },
      { id: 'height', text: '2.7' },
      { id: 'layer', text: 'Shell' },
    ]),
    wallSchema,
  )

  // The SCHEMA's order, not the order the values happened to arrive in and not
  // the alphabetical order a map would give.
  assert.deepEqual(
    sections.map((section) => section.group),
    ['Identity', 'Geometry'],
  )
  assert.deepEqual(
    sections[0].rows.map((row) => row.label),
    ['ID', 'Layer'],
  )
  assert.deepEqual(
    sections[1].rows.map((row) => row.label),
    ['Thickness', 'Height'],
  )
  assert.equal(sections[1].rows[0].unit, 'm')
})

test('a setting the runtime did not send is omitted and counted, never drawn blank', () => {
  // ⚠️ UNREAD IS NOT EMPTY. A blank row would make "this build cannot read a
  // wall's height" look exactly like "this wall is zero high", and nothing on
  // the panel would say which.
  const element = described([{ id: 'thickness', text: '0.3' }])
  const sections = settingSectionsOf(element, wallSchema)

  assert.equal(sections.length, 1)
  assert.deepEqual(
    sections[0].rows.map((row) => row.id),
    ['thickness'],
  )
  assert.equal(unreadSettingCount(element, wallSchema), 4)
})

test('a schema the response did not carry renders nothing rather than raw ids', () => {
  // The labels arrive with the values; without them there is no honest way to
  // name a row, and "thickness: 0.3" in a panel that elsewhere says "Thickness"
  // is a leaked implementation detail.
  const element = described([{ id: 'thickness', text: '0.3' }])
  assert.deepEqual(settingSectionsOf(element, undefined), [])
  assert.equal(unreadSettingCount(element, undefined), 0)
})

// --- container nodes --------------------------------------------------------

test('a container node is recognised through the catalog, not a hard-coded name', () => {
  assert.equal(containerElementType('archicad.container.wall', catalog), 'wall')
  // A classified type with no container is not one ...
  assert.equal(containerElementType('archicad.container.text', catalog), '')
  // ... nor is a plausible id for a type the runtime never sent.
  assert.equal(containerElementType('archicad.container.banana', catalog), '')
  assert.equal(containerElementType('archicad.getSelection', catalog), '')
  assert.equal(containerElementType('archicad.container.wall', []), '')
})

test('a container draws what it produced, and nothing before it has run', () => {
  const groups = containerGroupOf(
    'wall',
    {
      valueType: 'list',
      items: [
        { valueType: 'archicadElementRef', text: 'w1' },
        { valueType: 'archicadElementRef', text: 'w2' },
      ],
    },
    catalog,
  )
  assert.equal(groups.length, 1)
  assert.equal(groups[0].label, 'Walls')
  assert.deepEqual(groups[0].guids, ['w1', 'w2'])

  // No result yet is NOT "the elements wired in": showing those would put the
  // unfiltered list under the filtered node's name.
  assert.deepEqual(containerGroupOf('wall', undefined, catalog), [])
  assert.deepEqual(containerGroupOf('wall', { valueType: 'list', items: [] }, catalog), [])
})

test('a container ignores list members that are not element references', () => {
  const groups = containerGroupOf(
    'wall',
    { valueType: 'list', items: [{ valueType: 'double', number: 4 }, { valueType: 'archicadElementRef', text: 'w1' }] },
    catalog,
  )
  assert.deepEqual(groups[0].guids, ['w1'])
})

test('a row that does not apply is not a row this build failed to read', () => {
  // ⚠️ THE DEFECT THIS CLOSES. A Basic wall has no composite, so the reader
  // correctly writes none - and the panel used to count that as a setting "this
  // build does not read yet", telling the user it was short when it was
  // complete. Only the row the structure actually selects can be missing.
  const basic = described([
    { id: 'elementId', text: 'W-01' },
    { id: 'structure', text: 'Basic' },
    { id: 'buildingMaterial', text: 'Concrete' },
    { id: 'length', text: '4.2' },
  ])
  assert.equal(unreadSettingCount(basic, structuredWallSchema), 0)

  // The same wall as a composite: now `composite` is the row that applies and
  // `buildingMaterial` is the one that does not.
  const composite = described([
    { id: 'elementId', text: 'W-02' },
    { id: 'structure', text: 'Composite' },
    { id: 'length', text: '4.2' },
  ])
  assert.equal(unreadSettingCount(composite, structuredWallSchema), 1)
})

test('an unread governing setting does not make every row it gates unread', () => {
  // Not knowing what structure a wall is means not knowing which material row
  // to expect. Counting all of them would blame the reader for three fields on
  // the strength of one.
  const element = described([
    { id: 'elementId', text: 'W-03' },
    { id: 'length', text: '4.2' },
  ])
  assert.equal(unreadSettingCount(element, structuredWallSchema), 1)
})

test('a row carries the origin of its value so a derived number says so', () => {
  // Archicad has no wall length; the number is arithmetic Tapioca did, and a
  // disagreement with a schedule means something different because of that.
  const sections = settingSectionsOf(
    described([
      { id: 'structure', text: 'Basic' },
      { id: 'length', text: '4.2' },
    ]),
    structuredWallSchema,
  )
  const rows = sections.flatMap((section) => section.rows)
  assert.equal(rows.find((row) => row.id === 'length')?.origin, 'derived')
  assert.equal(rows.find((row) => row.id === 'structure')?.origin, 'archicad')
})

// --- the promotable property list -------------------------------------------

test('the property list is the type schema, not one row per element', () => {
  // ⚠️ §12. Four hundred walls have ONE answer to "what does a wall have".
  const list = propertyListOf(structuredWallSchema, [
    described([{ id: 'structure', text: 'Basic' }, { id: 'buildingMaterial', text: 'Concrete' }]),
    described([{ id: 'structure', text: 'Basic' }, { id: 'buildingMaterial', text: 'Concrete' }]),
  ])
  const rows = list?.groups.flatMap((group) => group.rows) ?? []
  assert.equal(rows.filter((row) => row.settingId === 'buildingMaterial').length, 1)
  assert.equal(list?.elementCount, 2)
})

test('a shared value previews as itself and a differing one as a count', () => {
  // A row that showed only the first element's value would be a quiet lie about
  // the other three.
  const list = propertyListOf(structuredWallSchema, [
    described([{ id: 'structure', text: 'Basic' }, { id: 'length', text: '4.2' }]),
    described([{ id: 'structure', text: 'Basic' }, { id: 'length', text: '3.1' }]),
  ])
  const rows = new Map((list?.groups.flatMap((g) => g.rows) ?? []).map((row) => [row.settingId, row]))
  assert.equal(rows.get('structure')?.preview, 'Basic')
  assert.equal(rows.get('length')?.preview, '2 values')
})

test('a row that cannot apply to any element is not offered for promotion', () => {
  // Promoting Composite on a set of Basic walls creates a node that answers
  // Absent for every element for ever.
  const list = propertyListOf(structuredWallSchema, [
    described([{ id: 'structure', text: 'Basic' }, { id: 'buildingMaterial', text: 'Concrete' }]),
  ])
  const ids = (list?.groups.flatMap((g) => g.rows) ?? []).map((row) => row.settingId)
  assert.equal(ids.includes('buildingMaterial'), true)
  assert.equal(ids.includes('composite'), false)
})

test('a mixed set offers the rows that are real for part of it', () => {
  const list = propertyListOf(structuredWallSchema, [
    described([{ id: 'structure', text: 'Basic' }, { id: 'buildingMaterial', text: 'Concrete' }]),
    described([{ id: 'structure', text: 'Composite' }, { id: 'composite', text: 'Cavity' }]),
  ])
  const ids = (list?.groups.flatMap((g) => g.rows) ?? []).map((row) => row.settingId)
  assert.equal(ids.includes('buildingMaterial'), true)
  assert.equal(ids.includes('composite'), true)
})

test('a setting that applies but was read for nobody is kept and marked', () => {
  // Unread is what this build cannot do YET. Hiding it makes the panel look
  // complete when it is short; §36's rule is that the shortfall is said.
  const list = propertyListOf(structuredWallSchema, [described([{ id: 'structure', text: 'Basic' }])])
  const row = (list?.groups.flatMap((g) => g.rows) ?? []).find((r) => r.settingId === 'length')
  assert.equal(row?.unread, true)
  assert.equal(row?.preview, '')
})

test('rows keep the runtime group order and carry unit and origin', () => {
  const list = propertyListOf(structuredWallSchema, [
    described([{ id: 'elementId', text: 'W-01' }, { id: 'structure', text: 'Basic' }, { id: 'length', text: '4.2' }]),
  ])
  assert.deepEqual(list?.groups.map((group) => group.group), ['Identity', 'Structure', 'Geometry'])
  const length = list?.groups.find((g) => g.group === 'Geometry')?.rows[0]
  assert.equal(length?.unit, 'm')
  assert.equal(length?.origin, 'derived')
})

test('an unreadable element contributes nothing rather than an empty row', () => {
  const gone: ElementDescription = { ...described([]), available: false, detail: 'deleted' }
  const list = propertyListOf(structuredWallSchema, [
    described([{ id: 'structure', text: 'Basic' }, { id: 'length', text: '4.2' }]),
    gone,
  ])
  const row = (list?.groups.flatMap((g) => g.rows) ?? []).find((r) => r.settingId === 'length')
  assert.equal(row?.preview, '4.2')
  assert.equal(row?.readCount, 1)
})

test('an element is named by what a person would recognise, never by its guid', () => {
  // ⚠️ THE POINT OF THE WHOLE ROW. "55614A45-AD1B-4CFD-..." tells nobody which
  // wall they are looking at, and four of them tell nobody four times.
  assert.equal(elementDisplayName(described([{ id: 'elementId', text: 'SW-001' }])), 'SW-001')
  assert.equal(elementDisplayName(described([{ id: 'zoneName', text: 'Kitchen' }])), 'Kitchen')
  assert.equal(elementDisplayName(described([{ id: 'libraryPart', text: 'Chair 01' }])), 'Chair 01')
  // Falls back to the type before the guid, because "Wall" is still an answer.
  assert.equal(elementDisplayName(described([])), 'Wall')
})

test('a count of differing values does not take the unit', () => {
  // ⚠️ "4 values m" reads as a quantity in metres. It is a tally of how many
  // distinct lengths there are, and a unit on it is a category error.
  const list = propertyListOf(structuredWallSchema, [
    described([{ id: 'structure', text: 'Basic' }, { id: 'length', text: '4.2' }]),
    described([{ id: 'structure', text: 'Basic' }, { id: 'length', text: '3.1' }]),
  ])
  const rows = new Map((list?.groups.flatMap((g) => g.rows) ?? []).map((row) => [row.settingId, row]))
  assert.equal(rows.get('length')?.sharedValue, false)
  assert.equal(rows.get('structure')?.sharedValue, true)
})

test('a unit follows a measurement and nothing else', () => {
  // ⚠️ "Polyline (2 points) m" reads as a length. The reference line's unit is
  // real - its coordinates are metres - but its PREVIEW is a shape summary, and
  // a unit after a summary is a category error rather than a formatting slip.
  const schema: ElementTypeSchema = {
    id: 'wall',
    label: 'Wall',
    plural: 'Walls',
    settings: [
      setting('height', 'Height', 'Geometry', 'double', 'm'),
      setting('boundsMin', 'Bounds Min', 'Geometry', 'point3', 'm'),
      setting('referenceLinePath', 'Reference Line', 'Geometry', 'polyline', 'm'),
    ],
  }
  const list = propertyListOf(schema, [
    described([
      { id: 'height', text: '3.20' },
      { id: 'boundsMin', text: '0.00, 0.00, 0.00' },
      { id: 'referenceLinePath', text: 'Polyline (2 points)' },
    ]),
  ])
  const rows = new Map((list?.groups.flatMap((g) => g.rows) ?? []).map((row) => [row.settingId, row]))
  assert.equal(rows.get('height')?.showUnit, true)
  assert.equal(rows.get('boundsMin')?.showUnit, true)
  assert.equal(rows.get('referenceLinePath')?.showUnit, false)
})
