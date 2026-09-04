/**
 * The node browser's logic, with no Svelte and no DOM in it.
 *
 * Everything here is a pure function of the catalog the runtime handed us, so
 * the ranking and the keyboard geometry can be tested offline - see
 * tests/nodeBrowser.test.ts. The dialog component owns only presentation and
 * pointer handling; a ranking rule that lives in a template cannot be checked.
 */
import type { NodeTypeSchema } from '../types'

/** The pseudo-category the tab row always starts with. */
export const ALL_CATEGORY = 'All'

/**
 * Why the browser is open. Only 'canvas' is implemented, but the request shape
 * is the one the connection-drag and replace-node gestures will need, so those
 * arrive as a new case rather than as a second dialog.
 */
export type NodeBrowserContext =
  | { mode: 'canvas' }
  | { mode: 'connection'; nodeId: string; portId: string; valueType: string }
  | { mode: 'replace'; nodeId: string }

export interface NodeBrowserRequest {
  /** Where the dialog is drawn, in canvas-relative pixels. */
  x: number
  y: number
  /**
   * Where a node created from this dialog lands, in flow space. FROZEN when the
   * dialog opens: typing in the search box must not move the node that typing
   * is about to create.
   */
  position: { x: number; y: number }
  context: NodeBrowserContext
}

export interface RankedEntry {
  schema: NodeTypeSchema
  /** Lower sorts first; see `scoreSchema`. */
  rank: number
}

/**
 * The tab row, derived from the catalog rather than declared.
 *
 * ⚠️ NOT A HARD-CODED LIST. The categories are the runtime's own strings - a
 * node type added in C++ carries its category with it, and a browser holding a
 * private enum would silently drop that whole category on the floor. The cost
 * is that the tab row grows if the runtime grows careless with category names,
 * which is a problem to fix in the runtime.
 */
export function browserCategories(catalog: NodeTypeSchema[]): string[] {
  const seen = new Set<string>()
  for (const schema of catalog) if (schema.category !== '') seen.add(schema.category)
  return [ALL_CATEGORY, ...[...seen].sort((a, b) => a.localeCompare(b))]
}

/** The text a schema is matched against, lower-cased once rather than per keystroke. */
export function searchTextOf(schema: NodeTypeSchema): string {
  return [
    schema.label,
    schema.nodeType,
    schema.category,
    schema.description,
    ...schema.inputs.map((port) => port.label),
    ...schema.outputs.map((port) => port.label),
  ]
    .join(' ')
    .toLocaleLowerCase()
}

/**
 * Whether `query`'s characters appear in `text` in order - the loose half of
 * the match, so "gtsl" finds "Get Selection".
 */
export function subsequenceMatch(text: string, query: string): boolean {
  let index = 0
  for (const character of query) {
    index = text.indexOf(character, index)
    if (index < 0) return false
    index += 1
  }
  return true
}

/**
 * How well one schema answers one query, as a sort key where LOWER IS BETTER.
 *
 * The bands are ordered by how sure we are the user meant this node: an exact
 * name, then a name that starts with what they typed, then a name that contains
 * it, then the node's id, then anything else in its metadata, and last the loose
 * subsequence. `undefined` means "no match at all". Descriptions never outrank
 * names - a long description mentioning "selection" must not push `Get
 * Selection` down the list.
 */
export function scoreSchema(schema: NodeTypeSchema, query: string): number | undefined {
  const needle = query.trim().toLocaleLowerCase()
  if (needle === '') return 0

  const label = schema.label.toLocaleLowerCase()
  if (label === needle) return 0
  if (label.startsWith(needle)) return 1
  if (label.includes(needle)) return 2

  const nodeType = schema.nodeType.toLocaleLowerCase()
  if (nodeType.startsWith(needle)) return 3
  if (nodeType.includes(needle)) return 4

  if (searchTextOf(schema).includes(needle)) return 5
  if (subsequenceMatch(label, needle)) return 6
  return undefined
}

/**
 * The entries one tab and one query select, best first.
 *
 * A category tab NARROWS the search rather than replacing it: the handoff's
 * "category selected + search = search within selected category". Ties break
 * alphabetically so the list a user learns the shape of does not reshuffle
 * between keystrokes.
 */
export function rankCatalog(
  catalog: NodeTypeSchema[],
  query: string,
  category: string,
): NodeTypeSchema[] {
  const ranked: RankedEntry[] = []
  for (const schema of catalog) {
    if (category !== ALL_CATEGORY && schema.category !== category) continue
    const rank = scoreSchema(schema, query)
    if (rank === undefined) continue
    ranked.push({ schema, rank })
  }
  ranked.sort((a, b) => a.rank - b.rank || a.schema.label.localeCompare(b.schema.label))
  return ranked.map((entry) => entry.schema)
}

/**
 * Where the arrow keys go, for a list laid out in columns of `rows`.
 *
 * The assortment fills TOP TO BOTTOM then rightwards (CSS `grid-auto-flow:
 * column`), so index i sits at column ⌊i/rows⌋ and row i%rows. Up/down walk the
 * flat index; left/right jump a whole column and clamp to the last entry rather
 * than wrapping, because wrapping in a two-dimensional list loses the user.
 */
export function moveSelection(
  index: number,
  count: number,
  rows: number,
  key: 'ArrowUp' | 'ArrowDown' | 'ArrowLeft' | 'ArrowRight',
): number {
  if (count === 0) return 0
  const safeRows = Math.max(1, rows)
  const step = key === 'ArrowUp' ? -1 : key === 'ArrowDown' ? 1 : key === 'ArrowLeft' ? -safeRows : safeRows
  return Math.max(0, Math.min(count - 1, index + step))
}

/**
 * The dialog's top-left, kept inside `viewport` with `margin` to spare.
 *
 * Clamped rather than flipped: a dialog opened near the bottom edge that jumps
 * ABOVE the cursor moves the list out from under the hand that is already
 * heading for it.
 */
export function clampDialogPosition(
  cursor: { x: number; y: number },
  dialog: { width: number; height: number },
  viewport: { width: number; height: number },
  margin = 12,
): { x: number; y: number } {
  return {
    x: Math.max(margin, Math.min(cursor.x, viewport.width - dialog.width - margin)),
    y: Math.max(margin, Math.min(cursor.y, viewport.height - dialog.height - margin)),
  }
}
