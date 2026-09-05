import { callTapioca } from '../../bridge.ts'
import type { ScriptCompletionItem, ScriptStatus } from './script.ts'

/**
 * The verbs, and nothing else.
 *
 * ⚠️ SEPARATED FROM script.ts AND buffer.ts SO THE RULES CAN BE TESTED WITHOUT A
 * BRIDGE. What a node should SAY about its file, and what an edited buffer may
 * do, are the parts that are easy to get subtly wrong and the parts that have no
 * business needing a running Archicad to check. Keeping the `window.EvP` import
 * out of those modules is what lets the offline suite cover them.
 *
 * ⚠️ READ AND WRITE ARE A PAIR AND ONLY WORK AS ONE. Read hands back the source
 * TOGETHER WITH a hash of exactly those bytes; write sends a buffer back with the
 * hash it started from, and the native side refuses it if disk has moved on since.
 * That is what lets the palette hold an editor while VSCode holds the same file:
 * neither can overwrite the other without the user being asked. Calling write with
 * a hash that did not come from a read of the same file defeats the whole thing.
 *
 * ⚠️ AND STATUS STILL CARRIES NO SOURCE TEXT. It is polled forever, once per
 * node on screen; the source crosses this bridge only when a buffer is opened.
 */

export async function fetchScriptStatus(nodeId: string, graphId?: string): Promise<ScriptStatus> {
  return callTapioca<ScriptStatus>('Tapioca.GraphScriptStatus', graphId === undefined ? { nodeId } : { nodeId, graphId })
}

export async function reloadScript(nodeId: string, graphId?: string): Promise<ScriptStatus> {
  return callTapioca<ScriptStatus>('Tapioca.GraphScriptReload', graphId === undefined ? { nodeId } : { nodeId, graphId })
}

export async function createScript(nodeId: string, path: string, graphId?: string): Promise<ScriptStatus> {
  return callTapioca<ScriptStatus>(
    'Tapioca.GraphScriptCreate',
    graphId === undefined ? { nodeId, path } : { nodeId, path, graphId },
  )
}

/** The source text of a node's file, with the hash a save must be guarded by. */
export interface ScriptReadResult {
  ok: boolean
  error: string
  /** The node's folder. */
  path: string
  /** The file within it, echoed back - see readScriptSource. */
  file: string
  shared: boolean
  language: string
  exists: boolean
  source: string
  /** Of the bytes in `source`, and of nothing else. See writeScriptSource. */
  sourceHash: string
  modifiedAtMs: number
  sizeBytes: number
}

export interface ScriptWriteResult {
  ok: boolean
  error: string
  /** The save was refused because disk had moved on. `diskSource` is that version. */
  conflict: boolean
  path: string
  file: string
  /** After a save, the hash of what was written; after a conflict, of what is on disk. */
  sourceHash: string
  diskSource: string
  modifiedAtMs: number
  sizeBytes: number
}

/**
 * One file out of the node's folder.
 *
 * `file` is a plain name inside that folder (`calculations.py`) or `libs/<name>`
 * in the shared library; an empty string means the entry file, which is how the
 * editor opens a node without first learning whether it is `main.py` or
 * `main.js`. Anything else - an absolute path, a `..`, a nested folder - is
 * refused natively, so there is no spelling here that reaches the filesystem at
 * large. The name is echoed back in the response, so a reply that lands after
 * the user switched tabs can be discarded instead of painted into the wrong one.
 */
export async function readScriptSource(nodeId: string, file = '', graphId?: string): Promise<ScriptReadResult> {
  return callTapioca<ScriptReadResult>(
    'Tapioca.GraphScriptRead',
    graphId === undefined ? { nodeId, file } : { nodeId, file, graphId },
  )
}

/**
 * Creates one EMPTY helper inside the node's own folder.
 *
 * Never in `libs/`: a helper made from inside one node's editor and silently
 * landing on every other node's import path is a surprise nobody wants twice.
 * Empty rather than templated, because a helper carrying `@in` / `@out`
 * directives would declare ports from a file that is not the entry point.
 */
export async function addScriptFile(nodeId: string, file: string, graphId?: string): Promise<void> {
  await callTapioca('Tapioca.GraphScriptAddFile', graphId === undefined ? { nodeId, file } : { nodeId, file, graphId })
}

/**
 * Saves a buffer back over the node's file.
 *
 * `baseHash` is the hash the buffer was opened on - from readScriptSource, or
 * from the previous successful write. The native side re-reads the file and
 * refuses the save if it no longer hashes to this, answering with `conflict` and
 * the other version rather than an error. That refusal is the feature: it is the
 * only thing standing between a palette save and an edit made in VSCode two
 * minutes ago.
 *
 * Ports are NOT reshaped by this. Reload does that, and the caller runs it after
 * a successful save, so there is one path by which a file changes a node.
 */
export async function writeScriptSource(
  nodeId: string,
  file: string,
  source: string,
  baseHash: string,
  graphId?: string,
): Promise<ScriptWriteResult> {
  return callTapioca<ScriptWriteResult>(
    'Tapioca.GraphScriptWrite',
    graphId === undefined ? { nodeId, file, source, baseHash } : { nodeId, file, source, baseHash, graphId },
  )
}

/** One node folder in the workflow library, as the script picker draws it. */
export interface ScriptLibraryEntry {
  /** The folder name, which is exactly what `scriptPath` holds. Never absolute. */
  name: string
  /** The `@name` from its entry file, when it has one. May be empty. */
  title: string
  /** False for a folder with no entry file — listed anyway, and marked. */
  hasEntry: boolean
  fileCount: number
  modifiedAtMs: number
}

export interface ScriptLibrary {
  ok: boolean
  error: string
  /** %LOCALAPPDATA%\Tapioca\Commands\Workflows, for the panel to name. */
  root: string
  language: string
  entries: ScriptLibraryEntry[]
  /**
   * A folder name nothing occupies yet. The name a brand-new node is scaffolded
   * under when the user first saves — see ScriptEditor: a script node writes
   * nothing to disk until then, so this is asked for at the same moment as the
   * template and answered by the same call.
   */
  suggestedName: string
  /**
   * The starter script, as text, from the native side that also writes it.
   *
   * ⚠️ NOT A COPY KEPT HERE. The buffer a new node opens on and the file that
   * eventually lands have to say the same thing, and a template duplicated in
   * this bundle is a template that drifts from the one Create writes.
   */
  template: string
}

/** What is in the workflow library, plus what a new node would be called. */
export async function fetchScriptLibrary(nodeId: string, graphId?: string): Promise<ScriptLibrary> {
  return callTapioca<ScriptLibrary>(
    'Tapioca.GraphScriptLibrary',
    graphId === undefined ? { nodeId } : { nodeId, graphId },
  )
}

/**
 * Sets the entry file's `@name`, which is the node's alias.
 *
 * ⚠️ IT EDITS THE USER'S FILE, WHICH IS WHY IT IS A NATIVE VERB AND NOT A BUFFER
 * EDIT HERE. The rename has to survive the file being open in VSCode at the same
 * time, so the native side reads, rewrites one line and writes back under the
 * hash it just read — refusing rather than overwriting if disk moved meanwhile.
 * Doing it through the editor's buffers would mean renaming a node silently
 * saved whatever the user had half-typed in the panel.
 */
export async function setScriptName(nodeId: string, name: string, graphId?: string): Promise<ScriptStatus> {
  return callTapioca<ScriptStatus>(
    'Tapioca.GraphScriptSetName',
    graphId === undefined ? { nodeId, name } : { nodeId, name, graphId },
  )
}

/**
 * Shows the workflow LIBRARY in Explorer — the folder every node folder is in.
 *
 * Distinct from revealScriptInExplorer, which shows one node's own file. This
 * one answers "where do my scripts live", needs no node to have a folder yet,
 * and creates the library if this machine has never had one deployed.
 */
export async function openScriptLibraryFolder(): Promise<void> {
  await callTapioca('Tapioca.GraphScriptOpenLibrary', {})
}

/**
 * Completion at a position in a node's buffer.
 *
 * ⚠️ THE BUFFER IS SENT, NOT READ FROM DISK, AND THAT IS THE WHOLE FEATURE.
 * Completion is asked for on text the user is midway through typing and has not
 * saved; a request that named only the file would complete against the version
 * before the edit being made.
 *
 * ⚠️ AND THE BROWSER NEVER SPEAKS LSP. It sends a position and gets labels back.
 * Relaying raw JSON-RPC would be less code and would give a browser inside
 * Archicad a way to ask a language server to read any file on the machine.
 *
 * `character` is a UTF-16 code-unit offset into the line — which is exactly what
 * a JavaScript string index is, so CodeMirror's own column goes across
 * unconverted. Do not "fix" this into a byte offset.
 */
export async function completeScript(
  nodeId: string,
  file: string,
  source: string,
  line: number,
  character: number,
  graphId?: string,
): Promise<{ ok: boolean; error: string; items: ScriptCompletionItem[] }> {
  const params = { nodeId, file, source, line, character }
  return callTapioca('Tapioca.GraphScriptComplete', graphId === undefined ? params : { ...params, graphId })
}

export type IntelligenceState = 'notInstalled' | 'installing' | 'ready' | 'failed'

export interface ScriptIntelligenceStatus {
  ok: boolean
  error: string
  state: IntelligenceState
  message: string
  /** Where the language server would be, so "not installed" can say what is missing. */
  executable: string
}

/** Whether code intelligence is available on this machine. */
export async function fetchIntelligenceStatus(): Promise<ScriptIntelligenceStatus> {
  return callTapioca<ScriptIntelligenceStatus>('Tapioca.GraphScriptIntelligence', {})
}

/**
 * Starts the on-demand install of the language server.
 *
 * Returns as soon as it has STARTED, not when it finishes: it is about 56 MB
 * over the network, and progress is watched by polling the status.
 */
export async function installIntelligence(): Promise<ScriptIntelligenceStatus> {
  return callTapioca<ScriptIntelligenceStatus>('Tapioca.GraphScriptInstallIntelligence', {})
}

export async function openScriptInEditor(nodeId: string, graphId?: string): Promise<void> {
  await callTapioca('Tapioca.GraphScriptOpen', graphId === undefined ? { nodeId } : { nodeId, graphId })
}

/**
 * Shows the file in Explorer, selected in its own folder.
 *
 * Distinct from openScriptInEditor, and worth both: `open` hands the file to
 * whatever edits code, which is what you want when you are writing the script.
 * This one shows you WHERE it is - which is what you want when you are looking
 * for the file, moving it, or checking that the path on the node points at the
 * thing you think it does.
 */
export async function revealScriptInExplorer(nodeId: string, graphId?: string): Promise<void> {
  await callTapioca('Tapioca.GraphScriptReveal', graphId === undefined ? { nodeId } : { nodeId, graphId })
}
