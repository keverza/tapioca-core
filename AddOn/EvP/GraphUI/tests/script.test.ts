import assert from 'node:assert/strict'
import test from 'node:test'
import { schemaForNode, schemaForNodeId } from '../src/nodes/types/schema.ts'
import { bodyModeFor } from '../src/nodes/types/display.ts'
import { capabilitiesFor, definitionFromSchema } from '../src/nodes/contracts.ts'
import {
  EMPTY_SCRIPT_STATUS,
  conditionOf,
  describeAge,
  fileNameOf,
  canRevealScript,
  isScriptNodeType,
  summaryOf,
  type ScriptStatus,
} from '../src/nodes/script/script.ts'
import type { GraphNodeRecord, NodeTypeSchema } from '../src/types.ts'

const scriptSchema: NodeTypeSchema = {
  nodeType: 'script.python',
  label: 'Python',
  category: 'Script',
  description: 'Runs a .py file from disk.',
  executionDomain: 'worker',
  display: 'script',
  instancePorts: true,
  inputs: [],
  outputs: [],
  parameters: [
    { parameterId: 'scriptPath', label: 'File', valueType: 'string', required: false },
    { parameterId: 'scriptLanguage', label: 'Runtime', valueType: 'string', required: false },
    { parameterId: 'scriptSourceHash', label: 'Loaded', valueType: 'string', required: false },
  ],
}

const addSchema: NodeTypeSchema = {
  nodeType: 'add',
  label: 'Add',
  category: 'Maths',
  description: 'Adds two numbers.',
  executionDomain: 'worker',
  inputs: [{ portId: 'left', label: 'Left', valueType: 'double' }],
  outputs: [{ portId: 'value', label: 'Value', valueType: 'double' }],
  parameters: [],
}

function scriptNode(overrides: Partial<GraphNodeRecord> = {}): GraphNodeRecord {
  return {
    nodeId: 'script1',
    nodeType: 'script.python',
    parameters: [],
    inputs: [{ portId: 'radius', label: 'Radius', valueType: 'double' }],
    outputs: [{ portId: 'area', label: 'Area', valueType: 'double' }],
    ...overrides,
  }
}

function status(overrides: Partial<ScriptStatus> = {}): ScriptStatus {
  return { ...EMPTY_SCRIPT_STATUS, ...overrides }
}

// ---------------------------------------------------------------------------
// The port-merge seam. These are the tests that matter most, because the way
// this goes wrong is silent: a reader that keeps using the catalog entry sees a
// script node with zero ports and draws a node that looks broken.

test('a script node takes its ports from its own state, not the catalog', () => {
  const merged = schemaForNode(scriptNode(), scriptSchema)
  assert.equal(merged?.inputs.length, 1)
  assert.equal(merged?.inputs[0].portId, 'radius')
  assert.equal(merged?.outputs[0].portId, 'area')
})

test('a node whose type does not declare instance ports is passed straight through', () => {
  const node: GraphNodeRecord = { nodeId: 'a1', nodeType: 'add', parameters: [] }
  assert.equal(schemaForNode(node, addSchema), addSchema)
})

test('ports carried by a node whose type never opted in are ignored', () => {
  // The mirror of the native rule: a hand-edited document must not be able to
  // bolt ports onto an Add. The catalog's own ports win.
  const node = scriptNode({ nodeId: 'a1', nodeType: 'add' })
  const merged = schemaForNode(node, addSchema)
  assert.equal(merged?.inputs[0].portId, 'left')
})

test('a script node whose header would not parse has no ports rather than the catalog\'s', () => {
  const merged = schemaForNode(scriptNode({ inputs: undefined, outputs: undefined }), scriptSchema)
  assert.deepEqual(merged?.inputs, [])
  assert.deepEqual(merged?.outputs, [])
})

test('schemaForNodeId answers undefined for a node the graph does not have', () => {
  const catalog = new Map([[scriptSchema.nodeType, scriptSchema]])
  assert.equal(schemaForNodeId('missing', [scriptNode()], catalog), undefined)
  assert.equal(schemaForNodeId('script1', [scriptNode()], catalog)?.inputs.length, 1)
})

// ---------------------------------------------------------------------------
// Presentation.

test('a script node draws a custom body and is resizable', () => {
  assert.equal(bodyModeFor(scriptSchema), 'custom')
  assert.equal(capabilitiesFor(scriptSchema).resizable, true)
})

test('a script node is not offered bypass, hold or a send button', () => {
  const capabilities = capabilitiesFor(scriptSchema)
  assert.equal(capabilities.bypass, false)
  assert.equal(capabilities.freeze, false)
  // Pure: a script sees its inputs and nothing else, so there is nothing to
  // commit to Archicad and no button for it.
  assert.equal(capabilities.execute, false)
})

test('a definition built from a merged schema carries the file-declared ports', () => {
  const definition = definitionFromSchema(schemaForNode(scriptNode(), scriptSchema)!)
  assert.equal(definition.inputs[0].portId, 'radius')
  assert.equal(definition.inputs[0].capabilities.internalise, true)
  assert.equal(definition.outputs[0].portId, 'area')
})

test('script node types are recognised by id', () => {
  assert.equal(isScriptNodeType('script.python'), true)
  assert.equal(isScriptNodeType('script.javascript'), true)
  assert.equal(isScriptNodeType('add'), false)
})

// ---------------------------------------------------------------------------
// What the node says about its file. The ORDER is the contract: a node can be
// several of these at once, and showing the wrong one first sends the user to
// look at the wrong thing.

test('no file beats every other condition', () => {
  assert.equal(conditionOf(status({ path: '', stale: true, diagnostics: [{ line: 1, message: 'x' }] })), 'empty')
})

test('a missing file beats a broken header', () => {
  const missing = status({ path: 'a.py', exists: false, diagnostics: [{ line: 2, message: 'bad type' }] })
  assert.equal(conditionOf(missing), 'missing')
})

test('a broken header beats a stale file', () => {
  const invalid = status({ path: 'a.py', exists: true, stale: true, diagnostics: [{ line: 2, message: 'bad type' }] })
  assert.equal(conditionOf(invalid), 'invalid')
  assert.match(summaryOf(invalid, 0), /1 problem in the header/)
})

test('a stale file reads differently depending on whether anything is watching', () => {
  const watched = status({ path: 'a.py', exists: true, stale: true, watching: true })
  const unwatched = status({ path: 'a.py', exists: true, stale: true, watching: false })
  assert.match(summaryOf(watched, 0), /Reloading/)
  // The one the user has to act on says so. Implying a save will be noticed when
  // nothing is watching is the failure this wording exists to prevent.
  assert.match(summaryOf(unwatched, 0), /press Reload/)
})

test('a loaded file reports when it was loaded', () => {
  const ready = status({ path: 'a.py', exists: true, loadedAtMs: 1_000_000 })
  assert.equal(conditionOf(ready), 'ready')
  assert.match(summaryOf(ready, 1_000_000), /Loaded just now/)
})

test('a load error is shown verbatim rather than replaced with a generic line', () => {
  const locked = status({ path: 'a.py', exists: true, loadError: 'could not open a.py; it may be locked' })
  assert.equal(conditionOf(locked), 'missing')
  assert.match(summaryOf(locked, 0), /may be locked/)
})

test('ages read the way a person would say them', () => {
  assert.equal(describeAge(0, 5_000), 'never')
  assert.equal(describeAge(5_000, 5_000), 'just now')
  assert.equal(describeAge(0 + 1, 31_000), '31s ago')
  assert.equal(describeAge(1_000, 121_000), '2m ago')
})

test('the node header shows the file name, on either separator', () => {
  assert.equal(fileNameOf('C:\\scripts\\offset.py'), 'offset.py')
  assert.equal(fileNameOf('/home/u/offset.js'), 'offset.js')
  assert.equal(fileNameOf('offset.py'), 'offset.py')
})

// ---------------------------------------------------------------------------
// Showing the file in Explorer.

test('the folder button needs a file that exists, not merely a path', () => {
  assert.equal(canRevealScript(status({ path: '', exists: false })), false)
  // The case that matters: a node whose file was renamed still holds the old
  // path, and that is exactly when someone reaches for this button. Revealing a
  // path that is not there can only produce an error, so it greys instead.
  assert.equal(canRevealScript(status({ path: 'C:\gone.py', exists: false })), false)
  assert.equal(canRevealScript(status({ path: 'C:\here.py', exists: true })), true)
})

test('a node with a broken header can still be shown in Explorer', () => {
  // Independent of whether the SCRIPT is usable: finding the file is how you go
  // and fix the header.
  const broken = status({ path: 'C:\a.py', exists: true, diagnostics: [{ line: 2, message: 'bad type' }] })
  assert.equal(conditionOf(broken), 'invalid')
  assert.equal(canRevealScript(broken), true)
})
