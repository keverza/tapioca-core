import type { EvaluationSummary, NodeResultRecord } from './types'

/* --- The commit surface ----------------------------------------------------
 *
 * ⚠️ "RUN" IS NOT A STEP, AND THIS MODULE MUST NOT LET THE UI PRETEND IT IS.
 * The solution is always live: every accepted edit schedules a pass, and that
 * pass never commits a host effect. So the pair on screen is not run/don't-run,
 * it is split by WHAT IS AT RISK - RUN re-evaluates and writes nothing, ACCEPT
 * is the only control in the editor that writes to the user's model.
 *
 * ⚠️ AND THE LIST ACCEPT COMMITS IS THE RUNTIME'S, NEVER THE BROWSER'S. The set
 * is exactly `skippedEffectNodes` from the most recent pass. Recomputing it here
 * by filtering the catalog on the effect kind would be a second, drifting copy
 * of the evaluation plan, and it would be wrong the moment a node is disabled,
 * bypassed, upstream of a failure, or gated by a condition the plan understood
 * and this file did not.
 */

/** The runtime's own code for "this node's effect was withheld, not refused". */
export const WITHHELD_CODE = 'node.blocked.sideEffectsWithheld'

export type CommitRowState = 'ready' | 'blocked' | 'unreported'

export interface CommitRow {
  nodeId: string
  /** The node's nickname if it has one, else its type's label. */
  name: string
  /** What it writes: the node type's own label from the catalog. */
  writes: string
  /**
   * The item count the runtime reported, or -1 when it reported none.
   *
   * Phrased as "items" wherever it is shown. "12 slabs will be created" is a
   * claim the runtime did not make.
   */
  count: number
  state: CommitRowState
  /** Prose from the runtime. Never composed here. */
  detail: string
}

/**
 * Why there is nothing to commit. Three different situations that a greyed-out
 * button would flatten into one.
 */
export type CommitBlocker =
  | 'none'
  | 'noRun'
  | 'noEffectfulNodes'
  | 'nothingWithheld'
  | 'allBlocked'

export interface CommitAssembly {
  /**
   * The revision the assembly was built at. If the graph has moved since, the
   * assembly is stale and must be rebuilt rather than committed - the
   * expected-revision idea applied to the one action where being wrong is
   * expensive.
   */
  revision: number
  rows: readonly CommitRow[]
  /** The ids ACCEPT will send as `targets`: the rows the user read as ready. */
  ready: readonly string[]
  blocker: CommitBlocker
}

export const EMPTY_ASSEMBLY: CommitAssembly = { revision: 0, rows: [], ready: [], blocker: 'noRun' }

export function buildCommitAssembly(
  summary: EvaluationSummary | null,
  results: readonly NodeResultRecord[],
  describe: (nodeId: string) => { name: string; writes: string },
  graphHasEffectfulNode: boolean,
): CommitAssembly {
  if (summary === null) return EMPTY_ASSEMBLY

  const byNode = new Map(results.map((result) => [result.nodeId, result]))
  const rows: CommitRow[] = (summary.skippedEffectNodes ?? []).map((nodeId) => {
    const result = byNode.get(nodeId)
    const { name, writes } = describe(nodeId)
    if (result === undefined) {
      // ⚠️ NOT ASSUMED READY. Committing a node the results payload does not
      // mention would mean writing to the model on the strength of a status
      // nobody reported.
      return { nodeId, name, writes, count: -1, state: 'unreported', detail: 'the runtime reported no status for this node' }
    }
    return {
      nodeId,
      name,
      writes,
      count: result.itemCount,
      state: result.code === WITHHELD_CODE ? 'ready' : 'blocked',
      detail: result.code === WITHHELD_CODE ? '' : result.message,
    }
  })

  const ready = rows.filter((row) => row.state === 'ready').map((row) => row.nodeId)
  return { revision: summary.revision, rows, ready, blocker: blockerFor(rows, ready, graphHasEffectfulNode) }
}

function blockerFor(rows: readonly CommitRow[], ready: readonly string[], graphHasEffectfulNode: boolean): CommitBlocker {
  if (rows.length === 0) return graphHasEffectfulNode ? 'nothingWithheld' : 'noEffectfulNodes'
  return ready.length === 0 ? 'allBlocked' : 'none'
}

/** Why ACCEPT is disabled, in the words the user needs rather than a grey button. */
export function commitBlockerText(blocker: CommitBlocker): string {
  switch (blocker) {
    case 'noRun':
      return 'Nothing has been evaluated yet.'
    case 'noEffectfulNodes':
      return 'This graph writes nothing to Archicad.'
    case 'nothingWithheld':
      return 'Everything this graph writes is already committed.'
    case 'allBlocked':
      return 'Every node waiting to commit is blocked.'
    default:
      return ''
  }
}

/**
 * Whether the assembly on screen still describes the graph.
 *
 * A commit against a list the user is no longer looking at is the one failure
 * this surface exists to prevent.
 */
export function assemblyIsStale(assembly: CommitAssembly, revision: number): boolean {
  return assembly.blocker !== 'noRun' && assembly.revision !== revision
}

export type CommitLamp = 'live' | 'running' | 'locked' | 'failed' | 'committed'

/**
 * The lamp carries the honesty a Run button used to imply.
 *
 * Order matters: a run in flight is reported as running even while the previous
 * pass is remembered as failed, and a locked solution outranks a stale success
 * because it is the reason the screen is not moving.
 */
export function commitLamp(state: {
  busy: boolean
  solutionLocked: boolean
  failed: boolean
  committed: boolean
}): CommitLamp {
  if (state.busy) return 'running'
  if (state.solutionLocked) return 'locked'
  if (state.failed) return 'failed'
  if (state.committed) return 'committed'
  return 'live'
}

/**
 * What to say after ACCEPT, from what the runtime answered rather than from the
 * fact that the call returned.
 *
 * ⚠️ `succeeded === true` WITH `effectsCommitted === false` IS A REFUSAL, NOT A
 * SUCCESS. Saying "committed" there would be a claim the model does not back up.
 */
export function commitOutcome(summary: EvaluationSummary, requested: readonly string[]): {
  committed: boolean
  failed: boolean
  message: string
} {
  if (!summary.succeeded)
    return { committed: false, failed: true, message: summary.error || 'The commit failed.' }
  if (summary.effectsCommitted === false) {
    const skipped = summary.skippedEffectNodes ?? []
    return {
      committed: false,
      failed: true,
      message: `Not committed: ${skipped.length > 0 ? skipped.join(', ') : 'the runtime declined to send'}`,
    }
  }
  return {
    committed: true,
    failed: false,
    message: `Committed ${requested.length} node${requested.length === 1 ? '' : 's'} to Archicad`,
  }
}
