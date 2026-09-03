import { callTapioca } from '../../bridge.ts'
import type { ScriptStatus } from './script.ts'

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
  path: string
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
  /** After a save, the hash of what was written; after a conflict, of what is on disk. */
  sourceHash: string
  diskSource: string
  modifiedAtMs: number
  sizeBytes: number
}

export async function readScriptSource(nodeId: string, graphId?: string): Promise<ScriptReadResult> {
  return callTapioca<ScriptReadResult>(
    'Tapioca.GraphScriptRead',
    graphId === undefined ? { nodeId } : { nodeId, graphId },
  )
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
  source: string,
  baseHash: string,
  graphId?: string,
): Promise<ScriptWriteResult> {
  return callTapioca<ScriptWriteResult>(
    'Tapioca.GraphScriptWrite',
    graphId === undefined ? { nodeId, source, baseHash } : { nodeId, source, baseHash, graphId },
  )
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
