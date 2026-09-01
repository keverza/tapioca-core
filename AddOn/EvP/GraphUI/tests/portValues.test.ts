import assert from 'node:assert/strict'
import test from 'node:test'
import { describeValueRule, graphValueFromText } from '../src/editor.ts'
import { parsePortReference, serializePortReference } from '../src/nodes/types/portReference.ts'

test('a typed-in value is encoded for its declared type or refused outright', () => {
  assert.deepEqual(graphValueFromText('double', ' 6.5 '), { valueType: 'double', number: 6.5 })
  assert.deepEqual(graphValueFromText('integer', '7'), { valueType: 'integer', number: 7 })
  assert.deepEqual(graphValueFromText('bool', 'TRUE'), { valueType: 'bool', bool: true })
  assert.deepEqual(graphValueFromText('bool', '0'), { valueType: 'bool', bool: false })
  assert.deepEqual(graphValueFromText('string', ' hi '), { valueType: 'string', text: 'hi' })
  assert.deepEqual(graphValueFromText('point3', '0, 0 1'), { valueType: 'point3', numbers: [0, 0, 1] })

  // Refused rather than sent and bounced: the runtime would reject each of
  // these, and its answer arrives two states after the keystroke.
  assert.equal(graphValueFromText('double', 'six'), undefined)
  assert.equal(graphValueFromText('integer', '7.5'), undefined)
  assert.equal(graphValueFromText('bool', 'maybe'), undefined)
  assert.equal(graphValueFromText('point3', '0, 1'), undefined)
  // A list comes out of a node; there is nothing to type.
  assert.equal(graphValueFromText('list', '1, 2, 3'), undefined)
  // An empty box means "leave it alone", not "clear it".
  assert.equal(graphValueFromText('double', '   '), undefined)

  assert.match(describeValueRule('point3'), /three numbers/)
  assert.match(describeValueRule('list'), /comes from a connection/)
})

test('a port reference round-trips, and ordinary pasted text is not one', () => {
  const reference = {
    kind: 'nodePort' as const,
    nodeId: 'multiply-1',
    portId: 'value',
    direction: 'output' as const,
    valueType: 'double',
  }
  assert.deepEqual(parsePortReference(serializePortReference(reference)), reference)

  // What a person actually meant to type stays a value.
  assert.equal(parsePortReference('12'), undefined)
  assert.equal(parsePortReference('some label'), undefined)
  assert.equal(parsePortReference('{ not json'), undefined)
  assert.equal(parsePortReference('{"kind":"other","nodeId":"a","portId":"b","direction":"output"}'), undefined)
  assert.equal(parsePortReference('{"kind":"nodePort","nodeId":"","portId":"b","direction":"output"}'), undefined)
  assert.equal(parsePortReference('{"kind":"nodePort","nodeId":"a","portId":"b","direction":"sideways"}'), undefined)
})
