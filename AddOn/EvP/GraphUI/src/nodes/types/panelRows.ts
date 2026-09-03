import type { NodeOutputRecord, OutputBranch } from '../../types'
import { summarizeValue } from './valueTree.ts'

/**
 * One line of a Panel node's readout.
 *
 * ⚠️ THE ROWS CARRY THE SHAPE, NOT JUST THE VALUES. A Panel exists to answer
 * "what is on this wire" - and half of that answer is whether the data is one
 * list or a tree of branches. Grasshopper's panel says so by drawing a path
 * header above each branch and an index beside each item; a flat run of lines
 * renders a grafted tree and a flattened one identically, which is precisely
 * the distinction every tree operation exists to make.
 */
export type PanelRow =
  /** A branch header - `{0;1}` - with how many items are under it. */
  | { kind: 'path'; key: string; label: string; summary: string }
  /** One item, at its index WITHIN ITS BRANCH, exactly as Grasshopper counts. */
  | { kind: 'item'; key: string; index: number; text: string; type: string }
  /** What the runtime capped, said out loud. Never silent. */
  | { kind: 'note'; key: string; text: string }

/** The root path, which every scalar and every plain list sits at. */
const ROOT = '{0}'

function itemRows(key: string, branch: OutputBranch): PanelRow[] {
  const items = branch.value?.items ?? []
  const rows: PanelRow[] = items.map((item, index) => ({
    kind: 'item',
    key: `${key}.${index}`,
    index,
    text: summarizeValue(item),
    type: item.valueType,
  }))
  if (items.length === 0 && branch.itemCount === 0) {
    rows.push({ kind: 'note', key: `${key}.empty`, text: '(empty branch)' })
    return rows
  }
  // The runtime spends one item budget ACROSS branches, so a branch can arrive
  // whole, cut, or not at all. A reader who is not told would read the cut as
  // the length.
  const hidden = Math.max(0, branch.itemCount - items.length)
  if (branch.truncated || hidden > 0) {
    rows.push({
      kind: 'note',
      key: `${key}.capped`,
      text: `${hidden} more item${hidden === 1 ? '' : 's'} not shown`,
    })
  }
  return rows
}

/**
 * The output's rows, branch by branch.
 *
 * ⚠️ PATH HEADERS APPEAR WHEN THERE IS A SHAPE TO REPORT. One branch at `{0}`
 * is what a scalar and an ordinary list both look like, so a `{0}` header there
 * would sit on nearly every panel in the graph saying nothing - and a reader who
 * learns to skip it skips it on the trees where it matters too.
 *
 * ⚠️ AND `branches: undefined` IS NOT "NO BRANCHES". A runtime older than the
 * tree layer sends no branches at all; those panels fall back to the runtime's
 * own rendered `text`, one line per item, exactly as they read before.
 */
export function panelRows(output: NodeOutputRecord | undefined): PanelRow[] {
  if (output === undefined) return []

  const branches = output.branches
  if (branches === undefined) {
    const lines = output.text === undefined || output.text === '' ? [] : output.text.split('\n')
    return lines.map((line, index) => ({
      kind: 'item',
      key: `line.${index}`,
      index,
      text: line,
      type: output.itemType ?? 'absent',
    }))
  }

  const declared = output.branchCount ?? branches.length
  const showPaths = declared > 1 || (branches.length > 0 && branches[0].path !== ROOT)

  const rows: PanelRow[] = []
  for (const branch of branches) {
    const key = `branch.${branch.segments.join('-')}`
    if (showPaths) {
      rows.push({
        kind: 'path',
        key,
        label: branch.path,
        summary: `${branch.itemCount} item${branch.itemCount === 1 ? '' : 's'}`,
      })
    }
    rows.push(...itemRows(key, branch))
  }

  if (output.branchesTruncated === true || declared > branches.length) {
    const hidden = Math.max(0, declared - branches.length)
    rows.push({
      kind: 'note',
      key: 'branch.capped',
      text: `${hidden} more branch${hidden === 1 ? '' : 'es'} not shown`,
    })
  }
  return rows
}

/**
 * What the panel says about the SHAPE, above the rows: a list, or a tree and how
 * many branches. This is the one-line answer to the question the node is placed
 * on a wire to ask.
 */
export function panelStructure(output: NodeOutputRecord | undefined): string {
  if (output === undefined) return ''
  const summary = output.summary ?? ''
  const declared = output.branchCount
  if (declared === undefined) return summary
  if (declared === 0) return summary === '' ? 'empty' : summary
  if (declared === 1) return summary === '' ? '1 branch' : summary
  return `${summary === '' ? 'tree' : summary} in ${declared} branches`
}
