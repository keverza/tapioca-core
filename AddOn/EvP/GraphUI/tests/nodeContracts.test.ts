import assert from 'node:assert/strict'
import test from 'node:test'
import { definitionFromSchema, presentationFor } from '../src/nodes/contracts.ts'
import { parseNodePresentations, serializeNodePresentations } from '../src/nodes/serialization.ts'
import { aggregateSeverity, formatDiagnostics } from '../src/nodes/types/diagnostics.ts'
import type { NodeTypeSchema } from '../src/types.ts'

const schema: NodeTypeSchema = {
  nodeType: 'viewer',
  label: 'Viewer',
  category: 'Display',
  description: 'Preview geometry',
  executionDomain: 'worker',
  display: 'preview',
  bypassMappings: [{ inputId: 'input', outputId: 'output' }],
  holdCapable: true,
  inputs: [{ portId: 'input', label: 'Input', valueType: 'geometry' }],
  outputs: [{ portId: 'output', label: 'Output', valueType: 'geometry' }],
  parameters: [],
}

test('native schema projects into a versioned capability-driven node definition', () => {
  const definition = definitionFromSchema(schema)
  assert.equal(definition.version, 1)
  assert.equal(definition.presentation.bodyMode, 'viewer')
  assert.equal(definition.capabilities.bypass, true)
  assert.equal(definition.capabilities.freeze, true)
  assert.equal(definition.inputs[0]?.capabilities.internalise, true)
  assert.equal(definition.outputs[0]?.capabilities.pasteReference, false)
  assert.equal(presentationFor({ ...schema, display: 'ports' }).viewer.enabled, false)
})

test('presentation serialization round-trips and migrates the legacy appearance map', () => {
  const visuals = new Map([
    ['node-1', { nickname: 'Facade', color: '#abcdef', viewMode: 'expanded' as const, portLayout: 'vertical' as const }],
  ])
  const parsed = parseNodePresentations(serializeNodePresentations(visuals))
  assert.equal(parsed.get('node-1')?.nickname, 'Facade')
  assert.equal(parsed.get('node-1')?.viewMode, 'expanded')
  assert.equal(parsed.get('node-1')?.portLayout, 'vertical')

  const migrated = parseNodePresentations({ legacy: { name: 'Old name', color: '#123456' } })
  assert.equal(migrated.get('legacy')?.nickname, 'Old name')
  assert.throws(() => parseNodePresentations({ version: 99, nodes: [] }), /Unsupported/)
})

test('diagnostics aggregate by severity and retain port context when copied', () => {
  const messages = [
    { severity: 'warning' as const, code: 'PORT-1', title: 'Missing data', nodeId: 'node', portId: 'input' },
    { severity: 'error' as const, code: 'NODE-2', title: 'Failed', nodeId: 'node' },
  ]
  assert.equal(aggregateSeverity(messages), 'error')
  assert.match(formatDiagnostics(messages), /PORT-1 Missing data \/ input/)
})
