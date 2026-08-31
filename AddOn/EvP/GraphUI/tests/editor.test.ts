import assert from 'node:assert/strict'
import test from 'node:test'
import type { Edge, Node } from '@xyflow/svelte'
import {
  detailLevelForZoom,
  filterCatalog,
  groupCatalog,
  initialTheme,
  isCatalogConnectionValid,
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
