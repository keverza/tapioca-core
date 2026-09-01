import assert from 'node:assert/strict'
import test from 'node:test'
import { bodyModeFor, categoryColor, nodeDisplayName } from '../src/nodes/types/display.ts'
import type { NodeTypeSchema } from '../src/types.ts'

function schema(display: NodeTypeSchema['display'], parameters = 0): NodeTypeSchema {
  return {
    nodeType: 'test',
    label: 'Test node',
    category: 'Geometry',
    description: 'Test',
    executionDomain: 'worker',
    display,
    inputs: [],
    outputs: [],
    parameters: Array.from({ length: parameters }, (_, index) => ({
      parameterId: `p${index}`,
      label: `Parameter ${index}`,
      valueType: 'number',
      required: false,
    })),
  }
}

test('body modes stay declarative and independent of concrete node types', () => {
  assert.equal(bodyModeFor(schema('ports')), 'none')
  assert.equal(bodyModeFor(schema('ports', 1)), 'parameters')
  assert.equal(bodyModeFor(schema('preview')), 'viewer')
  assert.equal(bodyModeFor(schema('preview', 1)), 'parameters+viewer')
  assert.equal(bodyModeFor(schema('selectionSet')), 'custom')

  // A node with no parameters but with inputs still gets the row body: its
  // unconnected inputs are typed into, so it needs somewhere to type.
  const withInput = schema('ports')
  withInput.inputs = [{ portId: 'left', label: 'Left', valueType: 'double' }]
  assert.equal(bodyModeFor(withInput), 'parameters')
})

test('appearance defaults are deterministic and nicknames remain optional', () => {
  const item = schema('ports')
  assert.equal(categoryColor('Geometry'), categoryColor('Geometry'))
  assert.equal(nodeDisplayName(item, '  My node  '), 'My node')
  assert.equal(nodeDisplayName(item, '  '), 'Test node')
})
