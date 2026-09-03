import assert from 'node:assert/strict'
import test from 'node:test'
import type { Edge, Node } from '@xyflow/svelte'
import {
  detailLevelForZoom,
  displayedOutputText,
  filterCatalog,
  groupCatalog,
  initialTheme,
  isCatalogConnectionValid,
  portTypesConnect,
  REFERENCE_EDGE_COLOR,
  REFERENCE_EDGE_STYLE,
} from '../src/editor.ts'
import type { NodeTypeSchema, SchemaNodeData } from '../src/types.ts'

const sourceSchema: NodeTypeSchema = {
  nodeType: 'number',
  label: 'Number',
  category: 'Values',
  description: 'A numeric source',
  executionDomain: 'worker',
  inputs: [],
  outputs: [{ portId: 'value', label: 'Value', valueType: 'number' }],
  parameters: [],
}

const targetSchema: NodeTypeSchema = {
  nodeType: 'panel',
  label: 'Panel',
  category: 'Display',
  description: 'Shows any value',
  executionDomain: 'worker',
  inputs: [{ portId: 'value', label: 'Input value', valueType: 'absent' }],
  outputs: [],
  parameters: [],
}

function graphNode(id: string, schema: NodeTypeSchema): Node<SchemaNodeData> {
  return { id, position: { x: 0, y: 0 }, data: { schema, parameters: [] } }
}

test('catalog search ANDs tokens across schema and ports', () => {
  assert.deepEqual(filterCatalog([sourceSchema, targetSchema], ''), [sourceSchema, targetSchema])
  assert.deepEqual(filterCatalog([sourceSchema, targetSchema], '   '), [sourceSchema, targetSchema])
  assert.deepEqual(filterCatalog([sourceSchema, targetSchema], 'display input'), [targetSchema])
  assert.deepEqual(filterCatalog([sourceSchema, targetSchema], 'numeric value'), [sourceSchema])
  assert.equal(groupCatalog([sourceSchema, targetSchema]).size, 2)
})

test('connection validation projects native types and multiplicity', () => {
  const nodes = [graphNode('source', sourceSchema), graphNode('target', targetSchema)]
  const connection = {
    source: 'source',
    sourceHandle: 'value',
    target: 'target',
    targetHandle: 'value',
  }
  assert.equal(isCatalogConnectionValid(connection, nodes, []), true)
  const occupied: Edge[] = [{ id: 'edge', source: 'source', target: 'target', targetHandle: 'value' }]
  assert.equal(isCatalogConnectionValid(connection, nodes, occupied), false)
  assert.equal(isCatalogConnectionValid({ ...connection, target: 'source' }, nodes, []), false)
})

test('contextual detail uses hysteresis at both thresholds', () => {
  assert.equal(detailLevelForZoom(0.49, 'normal'), 'compact')
  assert.equal(detailLevelForZoom(0.54, 'compact'), 'compact')
  assert.equal(detailLevelForZoom(0.6, 'compact'), 'normal')
  assert.equal(detailLevelForZoom(1.21, 'normal'), 'detailed')
  assert.equal(detailLevelForZoom(1.1, 'detailed'), 'detailed')
  assert.equal(detailLevelForZoom(1.0, 'detailed'), 'normal')
})

test('theme preference defaults safely to system', () => {
  assert.equal(initialTheme(undefined), 'system')
  assert.equal(initialTheme({ getItem: () => 'dark' }), 'dark')
  assert.equal(initialTheme({ getItem: () => 'invalid' }), 'system')
})

test('a pasted reference uses a dotted neutral connection treatment', () => {
  assert.match(REFERENCE_EDGE_STYLE, /stroke-dasharray/)
  assert.match(REFERENCE_EDGE_STYLE, new RegExp(REFERENCE_EDGE_COLOR))
})

test('connected inputs display typed upstream values rather than source names', () => {
  assert.equal(displayedOutputText({ portId: 'value', value: { valueType: 'double', number: 12.5 }, text: '12.5', summary: 'Number' }), '12.5')
  assert.equal(displayedOutputText({ portId: 'value', value: { valueType: 'bool', bool: false }, text: 'False', summary: 'Boolean' }), 'False')
  assert.equal(displayedOutputText({ portId: 'value', value: { valueType: 'string', text: '' }, text: '', summary: 'String' }), '""')
  assert.equal(displayedOutputText(undefined), 'Not evaluated')
})

test('the connection rule mirrors the runtime: wildcards both ways, and integer widens', () => {
  // The wildcard on the INPUT, which is what an inspector port has always been.
  assert.equal(portTypesConnect('mesh', 'absent'), true)
  // ...and on the OUTPUT, which tree.graft has since it cannot know its item type.
  assert.equal(portTypesConnect('absent', 'list'), true)
  assert.equal(portTypesConnect('double', 'double'), true)

  // Widening travels a wire; narrowing needs math.toInteger, because 2.5 is 2
  // or 3 depending on an answer only the author has.
  assert.equal(portTypesConnect('integer', 'double'), true)
  assert.equal(portTypesConnect('double', 'integer'), false)

  assert.equal(portTypesConnect('mesh', 'double'), false)
})
