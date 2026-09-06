import { callTapioca } from './bridge'
import { layoutFromPositions, type DockState, type LayoutRecord } from './editor'
import type {
  CameraAction,
  CameraActionOutcome,
  GraphRunState,
  PositionStore,
  SelectionAction,
  SelectionActionOutcome,
} from './types'

/**
 * The workflow library: saved graphs, as files the runtime owns.
 *
 * The editor is a client of this, not the owner of it. A graph lives in
 * `%LOCALAPPDATA%\Tapioca\Workflows`, and everything here is a thin typed call
 * onto the native `Tapioca.GraphLibrary*` verbs.
 *
 * The pure half - the name rule and the layout conversion - lives in editor.ts,
 * which imports nothing at runtime and is therefore the half the offline test
 * runner can load.
 */

export interface StoredGraphInfo {
  name: string
  label: string
  description: string
  nodeCount: number
}

export interface LibraryListing {
  /** Where the files actually are, so a support question is answerable. */
  location: string
  graphs: StoredGraphInfo[]
}

interface StoreOutcome {
  ok: boolean
  status: 'ok' | 'notFound' | 'invalid' | 'ioFailed' | 'invalidId'
  error: string
  name: string
  graphId: string
}

export interface LoadOutcome extends StoreOutcome {
  revision: number
  label: string
  description: string
  nodeLayout: LayoutRecord[]
}

/**
 * What the ordinary Windows Save/Open dialog answered.
 *
 * `cancelled` is the common case and is not an error: the user pressed Escape.
 * `outsideLibrary` is the one that needs saying out loud - the dialog opens in
 * the library folder but does not lock the user into it, and a workflow written
 * anywhere else would never appear in the library again.
 */
export interface BrowseOutcome {
  ok: boolean
  status: 'ok' | 'cancelled' | 'outsideLibrary' | 'invalid' | 'noLocation'
  error: string
  name: string
  location: string
}

/**
 * The native chooser, which is a PALETTE service rather than a runtime verb.
 *
 * Not every host has one - a standalone browser has no palette at all, and the
 * DG::Browser bridge does not intercept it - so a caller must be ready for this
 * to reject with "unknown command" and fall back to the in-page dialog.
 */
export async function browseForGraph(mode: 'save' | 'load', name = ''): Promise<BrowseOutcome> {
  return callTapioca<BrowseOutcome>('Tapioca.GraphLibraryBrowse', { mode, name })
}

export async function listGraphs(): Promise<LibraryListing> {
  return callTapioca<LibraryListing>('Tapioca.GraphLibraryList')
}

export async function saveGraph(
  name: string,
  label: string,
  positions: PositionStore,
  nodeIds: Iterable<string>,
  docks: ReadonlyMap<string, DockState> = new Map(),
): Promise<StoreOutcome> {
  return callTapioca<StoreOutcome>('Tapioca.GraphLibrarySave', {
    name,
    label,
    // ⚠️ THE DOCKS TRAVEL WITH THE GRAPH, NOT IN localStorage. A promoted row is
    // part of what the workflow IS; a graph opened on another machine that drew
    // every promotion as a loose box would be the same graph rendered as
    // something nobody would recognise.
    nodeLayout: layoutFromPositions(positions, nodeIds, docks),
  })
}

export async function loadGraph(name: string): Promise<LoadOutcome> {
  return callTapioca<LoadOutcome>('Tapioca.GraphLibraryLoad', { name })
}

export async function deleteGraph(name: string): Promise<StoreOutcome> {
  return callTapioca<StoreOutcome>('Tapioca.GraphLibraryDelete', { name })
}

/**
 * One of the selection set's five actions.
 *
 * A button press, not an evaluation. The runtime changes the set and then
 * evaluates the terminal nodes downstream of it, so the result of pressing
 * Update is on screen without anybody pressing Evaluate.
 */
export async function applySelectionAction(
  nodeId: string,
  action: SelectionAction,
): Promise<SelectionActionOutcome> {
  return callTapioca<SelectionActionOutcome>('Tapioca.GraphSelectionAction', { nodeId, action })
}

/**
 * One of the camera list's four actions.
 *
 * A button press, not an evaluation - the same contract the selection actions
 * carry. `index` names the row for Restore and Remove and is ignored by the
 * other two; the runtime REFUSES an index past the end rather than clamping it,
 * because a clamped Restore points the 3D window at a camera the user did not
 * click.
 */
export async function applyCameraAction(
  nodeId: string,
  action: CameraAction,
  index: number,
): Promise<CameraActionOutcome> {
  return callTapioca<CameraActionOutcome>('Tapioca.GraphCameraAction', { nodeId, action, index })
}

/**
 * Start a run on the runtime's OWN thread, and ask how it is going.
 *
 * ⚠️ THE ONLY WAY TO COMMIT A LONG NODE FROM THE EDITOR. Tapioca.GraphEvaluate
 * runs inline on the caller's thread, which here is Archicad's UI thread - fine
 * for a graph of arithmetic, a deadlock for a node that needs the main-thread
 * gate. See the native command's own note.
 */
export async function startRun(targets: string[], allowSideEffects: boolean): Promise<{ started: boolean; error: string }> {
  return callTapioca<{ started: boolean; error: string }>('Tapioca.GraphRunAsync', { targets, allowSideEffects })
}

export async function runState(): Promise<GraphRunState> {
  return callTapioca<GraphRunState>('Tapioca.GraphRunState')
}
