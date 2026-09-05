/**
 * What a script node knows about its file, and what it should say about it.
 *
 * ⚠️ NO BRIDGE IMPORT HERE, ON PURPOSE - the verbs live in scriptBridge.ts.
 * The rules below are the part that is easy to get subtly wrong (which condition
 * wins when a node is several at once; what a stale file reads as when nothing is
 * watching) and the part with no business needing a running Archicad to check.
 * Keeping `window.EvP` out of this module is what lets the offline suite cover it.
 *
 * ⚠️ AND THE SOURCE TEXT NEVER CROSSES THE BRIDGE. Status carries the path, the
 * runtime, the stamps, the parsed ports, the diagnostics and the last run's log -
 * everything needed to DRAW the node - and not its contents. A browser holding
 * the source is one text area away from being the second editor this whole design
 * exists to avoid.
 */

export interface ScriptPortInfo {
  portId: string
  label: string
  valueType: string
  /** The word the user typed in their own header - "number", not "double". */
  typeWord: string
  required?: boolean
}

export interface ScriptDiagnostic {
  /** 1-based, or 0 when the problem is the file as a whole. */
  line: number
  message: string
}

/** One file in a node's folder, as the editor draws it in the tab strip. */
export interface ScriptWorkspaceFile {
  /** `main.py`, or `libs/geometry.py` for a shared one. Never an absolute path. */
  name: string
  entry: boolean
  /**
   * From the shared library rather than this node's folder. Marked in the tab
   * strip, because an edit to it reaches every node that imports it - which is
   * exactly the change someone makes without meaning to.
   */
  shared: boolean
  sizeBytes: number
}

export interface ScriptEdgeRef {
  sourceNode: string
  sourcePort: string
  targetNode: string
  targetPort: string
}

export interface ScriptStatus {
  ok: boolean
  error: string
  graphId: string
  nodeId: string
  path: string
  language: string
  exists: boolean
  /** The file on disk has moved on since this node loaded it. */
  stale: boolean
  /**
   * Whether the native side is watching for saves. Reported rather than assumed,
   * because it changes what the node may promise: without a watcher a save is
   * picked up on the next evaluation or on Reload, and the node says so instead
   * of implying it is watching.
   */
  watching: boolean
  loadedAtMs: number
  modifiedAtMs: number
  sizeBytes: number
  /** Why the last load failed - a missing file, a locked one. Not a header error. */
  loadError: string
  name: string
  description: string
  inputs: ScriptPortInfo[]
  outputs: ScriptPortInfo[]
  diagnostics: ScriptDiagnostic[]
  log: string[]
  /** Wires a reload removed because their port no longer exists or changed type. */
  droppedEdges: ScriptEdgeRef[]
  interfaceChanged: boolean
  revision: number

  /** The node's FOLDER, as the document holds it. `path` is the same value. */
  workspaceRoot: string
  /** `<folder>/main.py`, resolved. Absolute; shown, never sent back. */
  entryFile: string
  /** Every source file in the folder, plus the shared library. The tab strip. */
  files: ScriptWorkspaceFile[]
  /** What the runtime puts on sys.path. Shown in the inspector's footer so an
   *  import that will not resolve can be diagnosed without guessing. */
  importRoots: string[]
  /**
   * Non-empty only when THIS reload converted a single-file node into a folder:
   * the path it used to hold. Said out loud because it MOVED the user's file -
   * the graph looks unchanged and the filesystem does not.
   */
  migratedFrom: string
}

export const EMPTY_SCRIPT_STATUS: ScriptStatus = {
  ok: false,
  error: '',
  graphId: '',
  nodeId: '',
  path: '',
  language: 'javascript',
  exists: false,
  stale: false,
  watching: false,
  loadedAtMs: 0,
  modifiedAtMs: 0,
  sizeBytes: 0,
  loadError: '',
  name: '',
  description: '',
  inputs: [],
  outputs: [],
  diagnostics: [],
  log: [],
  droppedEdges: [],
  interfaceChanged: false,
  revision: 0,
  workspaceRoot: '',
  entryFile: '',
  files: [],
  importRoots: [],
  migratedFrom: '',
}

export function isScriptNodeType(nodeType: string): boolean {
  return nodeType === 'script.javascript' || nodeType === 'script.python'
}

/**
 * How often to ask a script node whether its file has changed.
 *
 * ⚠️ THIS IS A FALLBACK, NOT THE MECHANISM. When the native watcher is running,
 * a save reloads the node before this ever fires and the poll only confirms what
 * already happened. It exists because a watcher can be absent (no implementation,
 * a folder that could not be opened) and because a notification can be missed -
 * and a node that silently stopped noticing saves is the single worst failure
 * this feature can have, since the user's next twenty minutes are spent debugging
 * code that is not the code being run.
 *
 * Only nodes on screen are polled, and only when the palette is focused - see
 * ScriptPanel.
 */
export const SCRIPT_POLL_INTERVAL_MS = 1500

/** A short "3 seconds ago" for the node's own status line. */
export function describeAge(atMs: number, nowMs: number): string {
  if (atMs <= 0) return 'never'
  const seconds = Math.max(0, Math.round((nowMs - atMs) / 1000))
  if (seconds < 5) return 'just now'
  if (seconds < 60) return `${seconds}s ago`
  const minutes = Math.round(seconds / 60)
  if (minutes < 60) return `${minutes}m ago`
  const hours = Math.round(minutes / 60)
  if (hours < 24) return `${hours}h ago`
  return `${Math.round(hours / 24)}d ago`
}

/** The file's own name, for the node header. Handles both separators. */
export function fileNameOf(path: string): string {
  const separator = Math.max(path.lastIndexOf('\\'), path.lastIndexOf('/'))
  return separator < 0 ? path : path.slice(separator + 1)
}

/**
 * What the node should SAY about its file, as one line, ranked by what the user
 * most needs to act on.
 *
 * ⚠️ ORDER MATTERS MORE THAN WORDING HERE. A node can be simultaneously stale,
 * carrying a header error and logging output; showing the wrong one of those
 * first sends the user to look at the wrong thing. A file that is missing beats a
 * header that is wrong, which beats a file that is newer than what ran - because
 * that is the order in which fixing one makes the next one visible.
 */
export type ScriptCondition = 'empty' | 'missing' | 'invalid' | 'stale' | 'ready'

export function conditionOf(status: ScriptStatus): ScriptCondition {
  if (status.path === '') return 'empty'
  if (status.loadError !== '' || !status.exists) return 'missing'
  if (status.diagnostics.length > 0) return 'invalid'
  if (status.stale) return 'stale'
  return 'ready'
}

/**
 * The node's own name for itself, for the header and the inspector's title.
 *
 * The header's `@name` wins, then the FOLDER's name - not the entry file's,
 * which is `main.py` on every node in the library and therefore tells nobody
 * anything.
 */
export function workspaceNameOf(status: ScriptStatus): string {
  if (status.name !== '') return status.name
  if (status.path === '') return ''
  const trimmed = status.path.replace(/[\\/]+$/u, '')
  return fileNameOf(trimmed)
}

export function summaryOf(status: ScriptStatus, nowMs: number): string {
  switch (conditionOf(status)) {
    case 'empty':
      /*
       * ⚠️ NOT AN ERROR, AND IT MUST NOT READ AS ONE. Since a script node is
       * scaffolded on its first save rather than on placement, this is the state
       * every script node is in for its first few seconds. "No folder yet" was
       * accurate and sounded like something had gone wrong; what the user needs
       * to know is which press ends it.
       */
      return 'Not created yet'
    case 'missing':
      return status.loadError === '' ? 'No main file in this folder' : status.loadError
    case 'invalid':
      return `${status.diagnostics.length} problem${status.diagnostics.length === 1 ? '' : 's'} in the header`
    case 'stale':
      return status.watching ? 'Reloading…' : 'Changed on disk — press Reload'
    case 'ready':
      return `Loaded ${describeAge(status.loadedAtMs, nowMs)}`
  }
}

/**
 * Whether "show in Explorer" can do anything yet.
 *
 * ⚠️ IT NEEDS A FILE THAT EXISTS, NOT MERELY A PATH. Revealing a path that is not
 * there can only produce an error, and an action offered and then refused is
 * worse than one that was never offered - so the button greys instead. `path`
 * alone is not enough: a node whose file has been renamed still holds the old
 * path, and that is exactly when someone reaches for this button.
 */
export function canRevealScript(status: ScriptStatus): boolean {
  return status.path !== '' && status.exists
}
