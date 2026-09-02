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
} from '../src/nodes/archicad/elements.ts'
import type { ElementDescription, ElementTypeSchema, GraphParameter } from '../src/types.ts'

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

const wallSchema: ElementTypeSchema = {
  id: 'wall',
  label: 'Wall',
  plural: 'Walls',
  settings: [
    { id: 'elementId', label: 'ID', group: 'Identity', valueType: 'string', unit: '' },
    { id: 'layer', label: 'Layer', group: 'Identity', valueType: 'string', unit: '' },
    { id: 'referenceLine', label: 'Reference Line', group: 'Placement', valueType: 'string', unit: '' },
    { id: 'thickness', label: 'Thickness', group: 'Geometry', valueType: 'double', unit: 'm' },
    { id: 'height', label: 'Height', group: 'Geometry', valueType: 'double', unit: 'm' },
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
