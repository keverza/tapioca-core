import { callTapioca } from '../../bridge.ts'
import type { ScriptStatus } from './script.ts'

/**
 * The four verbs, and nothing else.
 *
 * ⚠️ SEPARATED FROM script.ts SO THE RULES CAN BE TESTED WITHOUT A BRIDGE. What
 * a node should SAY about its file - which condition wins, how a stale file reads
 * when nothing is watching - is the part that is easy to get subtly wrong and the
 * part that has no business needing a running Archicad to check. Keeping the
 * `window.EvP` import out of that module is what lets the offline suite cover it.
 *
 * ⚠️ AND THERE IS NO WRITE VERB HERE, DELIBERATELY. The file belongs to VSCode or
 * Sublime; the moment the palette can write it, one save from the browser
 * silently discards whatever the user did in their editor since the node loaded.
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
