/**
 * The edited-buffer rules.
 *
 * ⚠️ THIS IS THE SUITE THAT STANDS BETWEEN THE PALETTE'S EDITOR AND SOMEONE'S
 * WORK. A script node's file is normally open in VSCode at the same time, so
 * every case below is a real sequence someone will hit within a day of using the
 * feature: edit here, save there; edit both; delete the file; save twice. What
 * these assert is mostly what does NOT happen - no version discarded without a
 * press, no prompt when there is nothing to decide.
 */

import assert from 'node:assert/strict'
import test from 'node:test'
import {
  EMPTY_BUFFER,
  applyConflict,
  applySaved,
  diffAgainstIncoming,
  editBuffer,
  editorLanguageOf,
  isDirty,
  keepLocal,
  openBuffer,
  phaseOf,
  reconcile,
  saveBlockedReason,
  takeDisk,
  EMPTY_TABS,
  anyConflict,
  bufferFor,
  dirtyFiles,
  newFileNameError,
  withBuffer,
  type ScriptBuffer,
} from '../src/nodes/script/buffer.ts'
import type { ScriptReadResult, ScriptWriteResult } from '../src/nodes/script/scriptBridge.ts'

function read(source: string, overrides: Partial<ScriptReadResult> = {}): ScriptReadResult {
  return {
    ok: true,
    error: '',
    // The node's FOLDER, which is what a read reports now; the file within it is
    // the separate `file` field.
    path: 'C:\\scripts\\offset',
    file: 'main.py',
    shared: false,
    language: 'python',
    exists: true,
    source,
    // The hash is opaque to every rule here - only ever compared - so the tests
    // use the text itself, which makes a wrong comparison legible in the failure.
    sourceHash: `hash:${source}`,
    modifiedAtMs: 1000,
    sizeBytes: source.length,
    ...overrides,
  }
}

function written(overrides: Partial<ScriptWriteResult> = {}): ScriptWriteResult {
  return {
    ok: true,
    error: '',
    conflict: false,
    path: 'C:\\scripts\\offset',
    file: 'main.py',
    sourceHash: 'hash:new',
    diskSource: '',
    modifiedAtMs: 2000,
    sizeBytes: 3,
    ...overrides,
  }
}

function opened(source = 'a = 1\n'): ScriptBuffer {
  return openBuffer(read(source))
}

test('a freshly opened buffer is clean and has nothing to save', () => {
  const buffer = opened()
  assert.equal(phaseOf(buffer), 'clean')
  assert.equal(isDirty(buffer), false)
  assert.equal(saveBlockedReason(buffer), 'Nothing to save.')
})

test('an empty buffer knows it has no file rather than offering to save one', () => {
  assert.equal(phaseOf(EMPTY_BUFFER), 'none')
  assert.equal(saveBlockedReason(EMPTY_BUFFER), 'This node has no file yet.')
})

test('typing makes the buffer dirty and saveable', () => {
  const buffer = editBuffer(opened(), 'a = 2\n')
  assert.equal(phaseOf(buffer), 'dirty')
  assert.equal(saveBlockedReason(buffer), '')
})

test('a file that changed under a CLEAN buffer is taken silently', () => {
  // The ordinary case: the user is editing in VSCode with the panel open beside
  // it. Prompting here would fire on every save they make in the editor this
  // feature exists to complement.
  const buffer = reconcile(opened(), read('a = 9\n'))
  assert.equal(buffer.text, 'a = 9\n')
  assert.equal(buffer.baseHash, 'hash:a = 9\n')
  assert.equal(phaseOf(buffer), 'clean')
})

test('a file that changed under a DIRTY buffer is a conflict, and nothing is discarded', () => {
  const mine = editBuffer(opened(), 'mine\n')
  const buffer = reconcile(mine, read('theirs\n'))
  assert.equal(phaseOf(buffer), 'conflict')
  // Both versions survive: the editor still shows the user's text, and disk's is
  // held for the choice.
  assert.equal(buffer.text, 'mine\n')
  assert.equal(buffer.incoming?.text, 'theirs\n')
})

test('a conflict outranks dirty, so the panel never offers a save that can only be refused', () => {
  const buffer = reconcile(editBuffer(opened(), 'mine\n'), read('theirs\n'))
  assert.equal(phaseOf(buffer), 'conflict')
  assert.equal(saveBlockedReason(buffer), 'Resolve the change on disk first.')
})

test('disk arriving at what the buffer already holds is not a conflict', () => {
  // Two ways this happens: the panel's own save coming back around through the
  // watcher, and the same one-line fix typed in both windows. Calling either a
  // conflict would be a dialog about two identical files.
  const mine = editBuffer(opened(), 'same\n')
  const buffer = reconcile(mine, read('same\n'))
  assert.equal(phaseOf(buffer), 'clean')
  assert.equal(buffer.baseHash, 'hash:same\n')
})

test('an unchanged hash is left completely alone', () => {
  const mine = editBuffer(opened(), 'mine\n')
  assert.equal(reconcile(mine, read('a = 1\n')), mine)
})

test('a failed read reports itself without touching the text', () => {
  const mine = editBuffer(opened(), 'mine\n')
  const buffer = reconcile(mine, read('', { ok: false, exists: false, error: 'no file at offset.py' }))
  assert.equal(buffer.text, 'mine\n')
  assert.equal(buffer.error, 'no file at offset.py')
})

test('keeping mine adopts disk\'s hash so the next save is allowed through', () => {
  // The guard's job is to make an overwrite deliberate; pressing Keep mine IS the
  // deliberation. Leaving the old base hash would refuse the very save just asked
  // for, and the user would press Save into a wall.
  const buffer = keepLocal(reconcile(editBuffer(opened(), 'mine\n'), read('theirs\n')))
  assert.equal(buffer.text, 'mine\n')
  assert.equal(buffer.baseHash, 'hash:theirs\n')
  assert.equal(phaseOf(buffer), 'dirty')
  assert.equal(saveBlockedReason(buffer), '')
})

test('taking disk replaces the buffer and leaves nothing to save', () => {
  const buffer = takeDisk(reconcile(editBuffer(opened(), 'mine\n'), read('theirs\n')))
  assert.equal(buffer.text, 'theirs\n')
  assert.equal(phaseOf(buffer), 'clean')
})

test('a save moves the base but never the text', () => {
  // A save is asynchronous and the user may type during it. Replacing the editor
  // with the bytes that were sent would silently drop those keystrokes; moving
  // only the base is what leaves the buffer correctly dirty again.
  const sent = 'sent\n'
  const typedSince = editBuffer(editBuffer(opened(), sent), 'sent and more\n')
  const buffer = applySaved(typedSince, written({ sourceHash: `hash:${sent}` }), sent)
  assert.equal(buffer.text, 'sent and more\n')
  assert.equal(buffer.baseText, sent)
  assert.equal(phaseOf(buffer), 'dirty')
})

test('a refused save becomes a conflict carrying the other version', () => {
  const mine = editBuffer(opened(), 'mine\n')
  const buffer = applyConflict(
    mine,
    written({ ok: false, conflict: true, diskSource: 'theirs\n', sourceHash: 'hash:theirs\n', error: 'the file changed on disk' }),
  )
  assert.equal(phaseOf(buffer), 'conflict')
  assert.equal(buffer.incoming?.text, 'theirs\n')
  assert.equal(buffer.error, 'the file changed on disk')
})

test('a file that vanished is a conflict with an empty other side', () => {
  // Deleting a script is deliberate. Recreating it because a buffer was open
  // would undo that without saying so, so the user is asked instead.
  const buffer = applyConflict(
    editBuffer(opened(), 'mine\n'),
    written({ ok: false, conflict: true, diskSource: '', sourceHash: '', error: 'the file is no longer there' }),
  )
  assert.equal(phaseOf(buffer), 'conflict')
  assert.equal(buffer.exists, false)
  assert.equal(buffer.incoming?.text, '')
})

test('the diff trims what both versions share and names the first changed line', () => {
  const mine = editBuffer(opened('one\ntwo\nthree\n'), 'one\nMINE\nthree\n')
  const buffer = reconcile(mine, read('one\nTHEIRS\nthree\n'))
  const diff = diffAgainstIncoming(buffer)
  assert.equal(diff?.firstChangedLine, 2)
  assert.deepEqual(diff?.mine, ['MINE'])
  assert.deepEqual(diff?.theirs, ['THEIRS'])
})

test('there is no diff when there is no conflict', () => {
  assert.equal(diffAgainstIncoming(opened()), null)
})

test('the editor mode follows the node, never the file extension', () => {
  // A node folder holds one language and the node type decides which. A stray
  // .js sitting beside main.py does not make that node a JavaScript node, so the
  // highlighter must not take the file's word for it.
  assert.equal(editorLanguageOf(opened()), 'python')
  assert.equal(editorLanguageOf(openBuffer(read('a', { language: 'javascript' }))), 'javascript')
  assert.equal(editorLanguageOf(openBuffer(read('a', { language: '', file: 'helpers.js' }))), 'python')
})

// ---------------------------------------------------------------------------
// Tabs. A node is a folder, so the inspector holds one buffer per open file.

test('tabs keep one buffer per file, and an unopened file is not an empty one', () => {
  const tabs = withBuffer(EMPTY_TABS, 'main.py', editBuffer(opened(), 'edited\n'))
  assert.equal(bufferFor(tabs, 'main.py').text, 'edited\n')
  // Absent means "not opened yet". Returning a blank buffer that looked editable
  // would let the user type into a file nobody has read, and save that over it.
  assert.equal(phaseOf(bufferFor(tabs, 'calculations.py')), 'none')
})

test('a conflict in one file does not block saving another', () => {
  // Per-file rules, deliberately. A model that merged them would refuse a save
  // to main.py because calculations.py is in conflict, which is nonsense to
  // anyone looking at main.py.
  const conflicted = reconcile(editBuffer(opened(), 'mine\n'), read('theirs\n'))
  let tabs = withBuffer(EMPTY_TABS, 'calculations.py', conflicted)
  tabs = withBuffer(tabs, 'main.py', editBuffer(opened(), 'fine\n'))
  assert.equal(saveBlockedReason(bufferFor(tabs, 'main.py')), '')
  assert.equal(anyConflict(tabs), true)
})

test('every unsaved file is named, because closing discards them all', () => {
  // The inspector's buffers die with the component and there is no undo across a
  // close. A user who edited three files and is looking at the fourth has to be
  // told which ones, by name.
  let tabs = withBuffer(EMPTY_TABS, 'main.py', editBuffer(opened(), 'a\n'))
  tabs = withBuffer(tabs, 'zebra.py', editBuffer(opened(), 'b\n'))
  tabs = withBuffer(tabs, 'clean.py', opened())
  assert.deepEqual(dirtyFiles(tabs), ['main.py', 'zebra.py'])
})

test('a new helper name is refused before it can reach the bridge', () => {
  const taken = ['main.py', 'calculations.py']
  assert.equal(newFileNameError('helpers.py', 'python', taken), '')
  assert.match(newFileNameError('', 'python', taken), /Name the file/u)
  assert.match(newFileNameError('helpers', 'python', taken), /\.py/u)
  assert.match(newFileNameError('.py', 'python', taken), /not just its extension/u)
  assert.match(newFileNameError('CALCULATIONS.PY', 'python', taken), /already/u)
  // The refusals that matter. This is a courtesy so the field can complain while
  // the user types - the native side refuses these too, and it is the real guard.
  for (const escape of ['../evil.py', '..\\evil.py', 'sub/nested.py', 'C:\\evil.py', '..', 'a\\b.py']) {
    assert.notEqual(newFileNameError(escape, 'python', taken), '', escape)
  }
})

test('a javascript node wants .js helpers', () => {
  assert.equal(newFileNameError('helpers.js', 'javascript', []), '')
  assert.match(newFileNameError('helpers.py', 'javascript', []), /\.js/u)
})
