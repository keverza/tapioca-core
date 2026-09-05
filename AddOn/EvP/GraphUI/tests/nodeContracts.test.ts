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

test('only bodies with a variable amount of content are resizable', () => {
  // ⚠️ THE REGRESSION THIS PINS: "more than two parameters" used to make a node
  // resizable, which handed a resize frame to every slider and picker on the
  // canvas - nodes whose height is decided entirely by their rows. The visible
  // symptom was an outline flickering around a Number node while its slider was
  // being dragged.
  const many = (count: number) =>
    Array.from({ length: count }, (_, index) => ({
      parameterId: `p${index}`,
      label: `P${index}`,
      valueType: 'double',
      required: false,
    }))

  const slider = { ...schema, display: 'default', parameters: many(5) } as NodeTypeSchema
  assert.equal(presentationFor(slider).resizable, false, 'a node full of parameters is not resizable')

  assert.equal(presentationFor(schema).resizable, true, 'a preview has a viewport with a size')
  assert.equal(presentationFor({ ...schema, display: 'text' } as NodeTypeSchema).resizable, true, 'a panel holds text')
  // ⚠️ AND THE SECOND REGRESSION: a script node used to be resizable because it
  // was assumed to hold an editor. It does not - the editor is the Script
  // Inspector, which takes the canvas full height and is resized on its own
  // edge. Everything on a script node body is as tall as it is going to get, so
  // the frame could only ever add empty space.
  assert.equal(
    presentationFor({ ...schema, display: 'script' } as NodeTypeSchema).resizable,
    false,
    'a script node body has no variable content; its editor is not drawn on it',
  )
})

test('the minimum size is what the body needs, not one number for every node', () => {
  // A floor below the content pushes it out through the node's own edges.
  const preview = presentationFor(schema).minSize
  const plain = presentationFor({ ...schema, display: 'default', parameters: [] } as NodeTypeSchema).minSize
  assert.ok(preview!.height > plain!.height, 'a viewport needs room to be a viewport')
  assert.ok(plain!.height > 70, 'header, ports and padding come to roughly 70px before the body draws')
})
