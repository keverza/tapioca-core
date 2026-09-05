import assert from 'node:assert/strict'
import test from 'node:test'

import {
  assemblyIsStale,
  buildCommitAssembly,
  commitBlockerText,
  commitLamp,
  commitOutcome,
  WITHHELD_CODE,
} from '../src/commit.ts'
import type { EvaluationSummary, NodeResultRecord } from '../src/types.ts'

function summary(over: Partial<EvaluationSummary> = {}): EvaluationSummary {
  return {
    graphId: 'default',
    runId: 1,
    revision: 118,
    succeeded: true,
    cancelled: false,
    error: '',
    failedNode: '',
    executedCount: 0,
    cacheHitCount: 0,
    failedCount: 0,
    blockedCount: 0,
    ...over,
  }
}

function result(nodeId: string, over: Partial<NodeResultRecord> = {}): NodeResultRecord {
  return {
    nodeId,
    status: 'blocked',
    code: WITHHELD_CODE,
    message: '',
    durationMilliseconds: 0,
    itemCount: 12,
    previewAvailable: false,
    ...over,
  }
}

const describe = (nodeId: string) => ({ name: nodeId, writes: 'Create Slab' })

test('the assembly is the runtime list, not an inference from the catalog', () => {
  // Two effectful nodes in the graph; the runtime withheld ONE. Only that one
  // is offered, because the plan knows things this module does not.
  const built = buildCommitAssembly(
    summary({ skippedEffectNodes: ['slab'] }),
    [result('slab'), result('wall')],
    describe,
    true,
  )
  assert.deepEqual(built.rows.map((row) => row.nodeId), ['slab'])
  assert.deepEqual(built.ready, ['slab'])
  assert.equal(built.blocker, 'none')
  assert.equal(built.revision, 118)
})

test('a blocked row is listed and is not offered for commit', () => {
  const built = buildCommitAssembly(
    summary({ skippedEffectNodes: ['slab', 'wall'] }),
    [result('slab'), result('wall', { code: 'node.blocked.upstream', message: 'upstream error in Filter Elements' })],
    describe,
    true,
  )
  assert.equal(built.rows.length, 2, 'hiding it is how a user believes they committed something they did not')
  assert.deepEqual(built.ready, ['slab'])
  assert.equal(built.rows[1].state, 'blocked')
  assert.equal(built.rows[1].detail, 'upstream error in Filter Elements')
})

test('a node the results payload does not mention is not assumed ready', () => {
  const built = buildCommitAssembly(summary({ skippedEffectNodes: ['ghost'] }), [], describe, true)
  assert.equal(built.rows[0].state, 'unreported')
  assert.equal(built.rows[0].count, -1)
  assert.deepEqual(built.ready, [])
  assert.equal(built.blocker, 'allBlocked')
})

test('an empty assembly says WHICH kind of empty it is', () => {
  const noRun = buildCommitAssembly(null, [], describe, true)
  assert.equal(noRun.blocker, 'noRun')

  const nothingEffectful = buildCommitAssembly(summary({ skippedEffectNodes: [] }), [], describe, false)
  assert.equal(nothingEffectful.blocker, 'noEffectfulNodes')

  const allCommitted = buildCommitAssembly(summary({ skippedEffectNodes: [] }), [], describe, true)
  assert.equal(allCommitted.blocker, 'nothingWithheld')

  // Three different situations, three different sentences.
  const said = new Set(
    (['noRun', 'noEffectfulNodes', 'nothingWithheld', 'allBlocked'] as const).map(commitBlockerText),
  )
  assert.equal(said.size, 4)
  assert.equal(commitBlockerText('none'), '')
})

test('an assembly built at another revision is stale', () => {
  const built = buildCommitAssembly(summary({ revision: 118, skippedEffectNodes: ['slab'] }), [result('slab')], describe, true)
  assert.equal(assemblyIsStale(built, 118), false)
  assert.equal(assemblyIsStale(built, 119), true)
  // "Nothing has run" is not staleness; there is no list to be stale.
  assert.equal(assemblyIsStale(buildCommitAssembly(null, [], describe, true), 42), false)
})

test('the lamp reports what is happening now over what happened last', () => {
  const state = { busy: false, solutionLocked: false, failed: false, committed: false }
  assert.equal(commitLamp(state), 'live')
  assert.equal(commitLamp({ ...state, committed: true }), 'committed')
  assert.equal(commitLamp({ ...state, failed: true, committed: true }), 'failed')
  assert.equal(commitLamp({ ...state, solutionLocked: true, failed: true }), 'locked')
  assert.equal(commitLamp({ ...state, busy: true, solutionLocked: true }), 'running')
})

test('succeeded with effectsCommitted false is a refusal, not a success', () => {
  const refused = commitOutcome(
    summary({ succeeded: true, effectsCommitted: false, skippedEffectNodes: ['slab'] }),
    ['slab'],
  )
  assert.equal(refused.committed, false)
  assert.equal(refused.failed, true)
  assert.match(refused.message, /Not committed: slab/)

  const failedRun = commitOutcome(summary({ succeeded: false, error: 'boom' }), ['slab'])
  assert.equal(failedRun.committed, false)
  assert.equal(failedRun.message, 'boom')

  const done = commitOutcome(summary({ succeeded: true, effectsCommitted: true }), ['slab', 'wall'])
  assert.equal(done.committed, true)
  assert.equal(done.message, 'Committed 2 nodes to Archicad')
})

test('the count is reported as items, and only when the runtime gave one', () => {
  const built = buildCommitAssembly(
    summary({ skippedEffectNodes: ['slab'] }),
    [result('slab', { itemCount: 40 })],
    describe,
    true,
  )
  assert.equal(built.rows[0].count, 40)
  assert.equal(built.rows[0].writes, 'Create Slab')
})
