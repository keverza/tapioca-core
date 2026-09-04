/**
 * The rules an edited script buffer follows, with no CodeMirror and no bridge in
 * sight.
 *
 * ⚠️ THIS IS THE PART THAT CAN LOSE SOMEONE'S WORK, WHICH IS WHY IT LIVES ALONE.
 * A script node's file is normally open in VSCode at the same time as the
 * palette's editor. Every question that decides whether an edit survives - is
 * this buffer dirty, may this save proceed, what happens when disk moved on while
 * the user was typing - is answered here, by functions that take a value and
 * return one. The Svelte component below does no reasoning of its own; it draws
 * what these say and calls the two bridge verbs.
 *
 * ⚠️ AND THE HASH, NOT THE STAMP, IS WHAT DECIDES. `baseHash` is the hash of the
 * bytes this buffer was opened on, and it is what the native side re-checks
 * before overwriting anything. A filesystem timestamp is coarse enough to miss a
 * fast save - good enough to ask "should I look again", never good enough to
 * answer "may I destroy this".
 */

import type { ScriptReadResult, ScriptWriteResult } from './scriptBridge.ts'

export interface ScriptBuffer {
  /**
   * The node FOLDER this buffer's file lives in. Which file it is, is the key it
   * is stored under - see ScriptTabs. Empty means "no buffer", which is what
   * every UI question below is really asking when it tests this.
   */
  path: string
  /** `python` or `javascript`, from the node - the editor picks its mode from it. */
  language: string
  /** Whether the file existed at the last read. A buffer for a missing file is
   *  still editable: saving it is how the file comes into existence. */
  exists: boolean
  /** What disk held when this buffer was opened, or last reconciled with it. */
  baseText: string
  baseHash: string
  /** What the user is looking at. */
  text: string
  /**
   * Disk's own version, held ONLY while a conflict is unresolved. Non-null is
   * exactly the state in which the user has a choice to make, so there is no
   * separate flag to keep in step with it.
   */
  incoming: { text: string; hash: string } | null
  /** Why the last read or save could not happen. Empty when nothing is wrong. */
  error: string
}

export const EMPTY_BUFFER: ScriptBuffer = {
  path: '',
  language: 'python',
  exists: false,
  baseText: '',
  baseHash: '',
  text: '',
  incoming: null,
  error: '',
}

/**
 * What the buffer is, as one word, for everything the UI decides.
 *
 * ⚠️ CONFLICT OUTRANKS DIRTY. A buffer in conflict is also dirty, and showing it
 * as merely dirty would offer a Save button whose only possible outcome is
 * another refusal - while the thing the user actually has to decide goes
 * unmentioned.
 */
export type BufferPhase = 'none' | 'conflict' | 'dirty' | 'clean'

export function phaseOf(buffer: ScriptBuffer): BufferPhase {
  if (buffer.path === '') return 'none'
  if (buffer.incoming !== null) return 'conflict'
  return buffer.text === buffer.baseText ? 'clean' : 'dirty'
}

export function isDirty(buffer: ScriptBuffer): boolean {
  return buffer.path !== '' && buffer.text !== buffer.baseText
}

/** Opens a buffer on what the native side just read. */
export function openBuffer(read: ScriptReadResult): ScriptBuffer {
  return {
    path: read.path,
    language: read.language,
    exists: read.exists,
    baseText: read.source,
    baseHash: read.sourceHash,
    text: read.source,
    incoming: null,
    error: read.ok ? '' : read.error,
  }
}

export function editBuffer(buffer: ScriptBuffer, text: string): ScriptBuffer {
  // An edit clears the last error: whatever was wrong, the user is now working,
  // and a stale complaint pinned over a buffer being typed into is noise.
  return { ...buffer, text, error: '' }
}

/**
 * What to do about a file that changed on disk while this buffer was open.
 *
 * The three outcomes, and why each is the only defensible one:
 *
 *   * the buffer is CLEAN - take disk silently. Nothing of the user's is at
 *     risk, and a prompt here would fire on every save they made in VSCode.
 *   * disk now matches what is in the buffer - adopt the hash and stop being
 *     dirty. This is the ordinary aftermath of THIS editor's own save, and of an
 *     identical edit made in both places; calling it a conflict would be a
 *     dialog about two files that are the same.
 *   * the buffer is DIRTY and disk differs - a conflict, held for the user.
 *     Neither version may be discarded by anything but a decision.
 */
export function reconcile(buffer: ScriptBuffer, read: ScriptReadResult): ScriptBuffer {
  if (!read.ok) return { ...buffer, exists: read.exists, error: read.error }
  if (read.sourceHash === buffer.baseHash) return buffer
  if (!isDirty(buffer) || read.source === buffer.text) {
    // `text` takes disk in both cases, which is a no-op in the second: a buffer
    // whose text already equals disk is being handed back what it holds.
    return { ...buffer, exists: read.exists, baseText: read.source, baseHash: read.sourceHash, text: read.source, incoming: null, error: '' }
  }
  return { ...buffer, exists: read.exists, incoming: { text: read.source, hash: read.sourceHash } }
}

/** A save that landed. The buffer's text is now what disk holds. */
export function applySaved(buffer: ScriptBuffer, result: ScriptWriteResult, savedText: string): ScriptBuffer {
  return {
    ...buffer,
    exists: true,
    baseText: savedText,
    baseHash: result.sourceHash,
    // ⚠️ THE TEXT IS NOT REPLACED. A save is asynchronous and the user may have
    // typed during it; overwriting the editor with the bytes that were sent would
    // silently drop those keystrokes. Only the BASE moves, which is exactly what
    // makes the buffer read as dirty again if they did type.
    incoming: null,
    error: '',
  }
}

/** A save that was refused because disk had moved on. */
export function applyConflict(buffer: ScriptBuffer, result: ScriptWriteResult): ScriptBuffer {
  return {
    ...buffer,
    exists: result.sourceHash !== '',
    incoming: { text: result.diskSource, hash: result.sourceHash },
    error: result.error,
  }
}

/**
 * Keep what is in the editor, and let the next save overwrite disk.
 *
 * ⚠️ THIS ADOPTS DISK'S HASH WITHOUT ADOPTING ITS TEXT, and that is the entire
 * meaning of the choice: the guard's job is to make sure an overwrite is
 * deliberate, and pressing this is the deliberation. Leaving the old base hash
 * would refuse the very save the user just asked for.
 */
export function keepLocal(buffer: ScriptBuffer): ScriptBuffer {
  if (buffer.incoming === null) return buffer
  return { ...buffer, baseHash: buffer.incoming.hash, baseText: buffer.incoming.text, incoming: null, error: '' }
}

/** Throw away the buffer's edits and take what is on disk. */
export function takeDisk(buffer: ScriptBuffer): ScriptBuffer {
  if (buffer.incoming === null) return buffer
  return {
    ...buffer,
    baseText: buffer.incoming.text,
    baseHash: buffer.incoming.hash,
    text: buffer.incoming.text,
    incoming: null,
    error: '',
  }
}

/**
 * The line-by-line difference between the buffer and disk, for the conflict view.
 *
 * Deliberately not a real diff algorithm: this is read to answer "what did the
 * other window change", over two versions of the same file minutes apart, and a
 * common-prefix/suffix trim answers that in the cases that actually occur while
 * being something anyone can verify at a glance. A three-way merge would be a
 * promise this feature should not make.
 */
export interface BufferDiff {
  firstChangedLine: number
  mine: string[]
  theirs: string[]
}

export function diffAgainstIncoming(buffer: ScriptBuffer): BufferDiff | null {
  if (buffer.incoming === null) return null
  const mine = buffer.text.split('\n')
  const theirs = buffer.incoming.text.split('\n')
  let start = 0
  while (start < mine.length && start < theirs.length && mine[start] === theirs[start]) start += 1
  let end = 0
  while (
    end < mine.length - start &&
    end < theirs.length - start &&
    mine[mine.length - 1 - end] === theirs[theirs.length - 1 - end]
  ) {
    end += 1
  }
  return {
    firstChangedLine: start + 1,
    mine: mine.slice(start, mine.length - end),
    theirs: theirs.slice(start, theirs.length - end),
  }
}

/**
 * Whether Save may be pressed, and what to say when it may not.
 *
 * A conflict is NOT a reason to disable Save - the user resolves it first and
 * the button says so - but an unresolved one must never reach the bridge, since
 * the native side would only refuse it again.
 */
export function saveBlockedReason(buffer: ScriptBuffer): string {
  if (buffer.path === '') return 'This node has no file yet.'
  if (buffer.incoming !== null) return 'Resolve the change on disk first.'
  if (!isDirty(buffer)) return 'Nothing to save.'
  return ''
}

/**
 * The CodeMirror language mode a buffer wants.
 *
 * From the NODE, not from the file's extension: a node folder holds one
 * language, the node type decides which, and `main.py` beside a stray `.js` does
 * not make that node a JavaScript node. Python is the fallback for a buffer
 * opened before the first status has arrived - it is the more common of the two
 * and, since this only picks a highlighter, being briefly wrong costs nothing.
 */
export function editorLanguageOf(buffer: ScriptBuffer): 'python' | 'javascript' {
  return buffer.language === 'javascript' ? 'javascript' : 'python'
}

// ---------------------------------------------------------------------------
// Tabs: one buffer per open file in the node's folder.
//
// ⚠️ THE RULES ABOVE ARE PER FILE AND STAY THAT WAY. A workspace is a map of
// them, not a bigger kind of buffer - a conflict in `calculations.py` has
// nothing to do with `main.py`, and a model that merged them would either block
// a save on an unrelated file or resolve two conflicts with one press.

export interface ScriptTabs {
  /**
   * The file the editor is showing. `''` means the entry file, which is what the
   * inspector opens with - it does not need to know whether that is `main.py` or
   * `main.js` before asking for it.
   */
  active: string
  /** Buffers by file name. Absent means "not opened yet", not "empty". */
  open: Record<string, ScriptBuffer>
}

export const EMPTY_TABS: ScriptTabs = { active: '', open: {} }

export function bufferFor(tabs: ScriptTabs, file: string): ScriptBuffer {
  return tabs.open[file] ?? EMPTY_BUFFER
}

export function withBuffer(tabs: ScriptTabs, file: string, buffer: ScriptBuffer): ScriptTabs {
  return { ...tabs, open: { ...tabs.open, [file]: buffer } }
}

/**
 * Every open file with unsaved edits.
 *
 * ⚠️ WHAT THIS IS FOR IS CLOSING. The inspector's buffers die with the component
 * and there is no undo across a close, so a user who edited three files and is
 * looking at the fourth has to be told about the other two BY NAME - "unsaved
 * changes" with no list is a prompt you dismiss and regret.
 */
export function dirtyFiles(tabs: ScriptTabs): string[] {
  return Object.keys(tabs.open)
    .filter((file) => isDirty(tabs.open[file] as ScriptBuffer))
    .sort()
}

export function anyConflict(tabs: ScriptTabs): boolean {
  return Object.values(tabs.open).some((buffer) => buffer.incoming !== null)
}

/**
 * Whether a new helper's name is one the node will accept, and why not.
 *
 * ⚠️ CHECKED HERE AS WELL AS NATIVELY, AND THE NATIVE ONE IS THE REAL GUARD.
 * This exists so the `+` field can refuse `../evil.py` while the user is typing
 * instead of after a round trip; it is a courtesy, not a boundary. Never remove
 * the native check on the strength of this one - a browser is not where a
 * filesystem rule gets to live.
 */
export function newFileNameError(name: string, language: 'python' | 'javascript', taken: string[]): string {
  const trimmed = name.trim()
  if (trimmed === '') return 'Name the file.'
  // Backslash FIRST in the class, and it is the one that matters most here: this
  // is Windows, `..\evil.py` is the natural spelling of an escape attempt, and a
  // character class that lost it would wave that through.
  if (/[\\/:*?"<>|]/u.test(trimmed)) return 'A file name, not a path.'
  if (trimmed === '.' || trimmed === '..') return 'A file name, not a path.'
  const extension = language === 'javascript' ? '.js' : '.py'
  if (!trimmed.toLowerCase().endsWith(extension)) return `Helpers end in ${extension}.`
  if (trimmed.length === extension.length) return 'Name the file, not just its extension.'
  if (taken.some((existing) => existing.toLowerCase() === trimmed.toLowerCase())) return 'There is already a file called that.'
  return ''
}
