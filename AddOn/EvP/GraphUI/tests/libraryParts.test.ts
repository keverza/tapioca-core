import assert from 'node:assert/strict'
import test from 'node:test'
import {
  ancestorKeys,
  buildFolderTree,
  decodeSelection,
  encodeSelection,
  filterParts,
  folderKey,
  LIBRARY_ROOT_KEY,
  matchesQuery,
  partsInFolder,
} from '../src/nodes/controls/libraryParts.ts'
import type { LibraryPartRow } from '../src/types.ts'

/**
 * ⚠️ WHAT THIS PROTECTS. The palette's browser records the failure that produced
 * its three-pane shape - folders and objects in ONE tree gave every object a
 * folder of its own - and the identity rule that a document name is not an
 * identity. Both are easy to lose in a port, and both fail QUIETLY: a browser
 * that groups wrongly still looks like a browser, and a selection stored by name
 * alone works until a second library loads the same name.
 */

function part(name: string, treePath: string[], extra: Partial<LibraryPartRow> = {}): LibraryPartRow {
  return {
    name,
    file: `${name}.gsm`,
    unID: `unid-${name}`,
    type: 'Object',
    location: `C:/lib/${name}.gsm`,
    placeable: true,
    missing: false,
    treePath,
    library: 'Archicad Library 29',
    embedded: false,
    ...extra,
  }
}

const catalog: LibraryPartRow[] = [
  part('Armchair 01', ['Loaded Libraries', 'Object Library', 'Furnishing', 'Chairs']),
  part('Stool 02', ['Loaded Libraries', 'Object Library', 'Furnishing', 'Chairs']),
  part('Table 03', ['Loaded Libraries', 'Object Library', 'Furnishing', 'Tables']),
  part('North Arrow', ['Loaded Libraries', 'Object Library', 'Annotation']),
  part('Loose Part', []),
]

test('the folder tree is the library manager path, and an intermediate level counts its subtree', () => {
  const tree = buildFolderTree(catalog)

  assert.equal(tree.length, 1)
  assert.equal(tree[0].label, 'Loaded Libraries')
  // Every part is under it, including the one with no path of its own.
  assert.equal(tree[0].totalCount, 5)
  // ... and it holds ONE part directly: the pathless one, which is filed there
  // rather than dropped.
  assert.equal(tree[0].count, 1)

  const objectLibrary = tree[0].children.find((child) => child.label === 'Object Library')
  assert.ok(objectLibrary)
  // ⚠️ AN INTERMEDIATE LEVEL HOLDS NOTHING ITSELF AND STILL SHOWS. Hiding it
  // would break the trail to everything beneath it.
  assert.equal(objectLibrary.count, 0)
  assert.equal(objectLibrary.totalCount, 4)
  assert.deepEqual(
    objectLibrary.children.map((child) => child.label),
    ['Annotation', 'Furnishing'],
  )
})

test('a folder shows its OWN parts, never its subtree', () => {
  // The rule that keeps a four-thousand-part library navigable: selecting
  // "Object Library" with descendants folded in would put the whole catalogue in
  // the right pane and undo the split the browser exists for.
  const objectLibrary = folderKey(['Loaded Libraries', 'Object Library'])
  assert.deepEqual(partsInFolder(catalog, objectLibrary), [])

  const chairs = folderKey(['Loaded Libraries', 'Object Library', 'Furnishing', 'Chairs'])
  assert.deepEqual(
    partsInFolder(catalog, chairs).map((row) => row.name),
    ['Armchair 01', 'Stool 02'],
  )
})

test('the root selection means everything, so the browser never opens on an empty pane', () => {
  assert.equal(partsInFolder(catalog, LIBRARY_ROOT_KEY).length, 5)
})

test('a part with no library-manager path is filed, not dropped', () => {
  // It is a real placeable object whose path the API did not answer for. Losing
  // it would make the browser quietly shorter than the library, with nothing to
  // say so.
  const loose = partsInFolder(catalog, folderKey(['Loaded Libraries']))
  assert.deepEqual(
    loose.map((row) => row.name),
    ['Loose Part'],
  )
})

test('search covers the folder path, not just the part name', () => {
  // "chairs" is how somebody looks for the Chairs folder; a search that matched
  // only names would answer "nothing" while the folder sits three rows away.
  assert.equal(matchesQuery(catalog[0], 'chairs'), true)
  assert.equal(matchesQuery(catalog[0], 'armchair'), true)
  assert.equal(matchesQuery(catalog[0], 'Armchair 01.gsm'), true)
  assert.equal(matchesQuery(catalog[0], 'window'), false)
  // An empty query matches everything rather than nothing.
  assert.equal(filterParts(catalog, '   ').length, 5)
})

test('a search narrows the tree as well as the contents', () => {
  // A tree that kept every folder while the right pane emptied would send the
  // user clicking through folders that no longer hold anything, with no
  // indication which ones still do.
  const tree = buildFolderTree(filterParts(catalog, 'chairs'))
  const objectLibrary = tree[0].children.find((child) => child.label === 'Object Library')
  assert.ok(objectLibrary)
  assert.deepEqual(
    objectLibrary.children.map((child) => child.label),
    ['Furnishing'],
  )
  assert.equal(tree[0].totalCount, 2)
})

test('ancestor keys reveal a search result in the tree', () => {
  assert.deepEqual(ancestorKeys(['a', 'b', 'c']), [folderKey(['a']), folderKey(['a', 'b'])])
  assert.deepEqual(ancestorKeys(['a']), [])
})

test('a stored selection carries the unique ID, not only the name', () => {
  // ⚠️ THE FAILURE WITH NO SYMPTOM. A document name is unique only in that
  // Archicad registers the NEWEST part carrying it; two loaded libraries shipping
  // "Armchair 01" leave one invisible. Storing the label alone works until the
  // day a second library loads.
  const encoded = encodeSelection(catalog[0])
  const decoded = decodeSelection(encoded)

  assert.ok(decoded)
  assert.equal(decoded.name, 'Armchair 01')
  assert.equal(decoded.unID, 'unid-Armchair 01')
  assert.equal(decoded.type, 'Object')
  assert.equal(decoded.file, 'Armchair 01.gsm')
})

test('an unset or unreadable value is "nothing chosen", never a thrown error', () => {
  // A graph that will not open is a worse outcome than a picker that lost its
  // selection - and a blob that will not parse can only come from a hand-edited
  // file.
  assert.equal(decodeSelection(undefined), undefined)
  assert.equal(decodeSelection(''), undefined)
  assert.equal(decodeSelection('   '), undefined)
  assert.equal(decodeSelection('not json at all'), undefined)
  assert.equal(decodeSelection('[1,2,3]'), undefined)
  assert.equal(decodeSelection('{"unID":"x"}'), undefined)
})

test('a value that names a part with no unique ID still decodes, so the control can say so', () => {
  // Dropping it would silently lose the user's choice; accepting it silently
  // would hide that the value is weaker than a properly recorded one. It decodes,
  // and the control renders a warning beside it.
  const decoded = decodeSelection('{"name":"Armchair 01"}')
  assert.ok(decoded)
  assert.equal(decoded.unID, '')
})
