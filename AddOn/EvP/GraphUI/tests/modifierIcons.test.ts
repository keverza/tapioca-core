import assert from 'node:assert/strict'
import test from 'node:test'
import { MODIFIER_ICONS, modifierIcon } from '../src/nodes/types/modifierIcons.ts'

// The badge is the answer to the one real objection to port modifiers - that a
// modifier you cannot see is a hidden node - so an icon that silently goes
// missing turns the feature back into the thing it was accused of being.

test('every modifier the runtime can report has a glyph', () => {
  // The runtime's vocabulary, from PortModifier in Graph.hpp. `none` is the
  // absence of a modifier and deliberately has no icon.
  for (const modifier of ['flatten', 'graft', 'simplify', 'reverse', 'round', 'normalise']) {
    const icon = modifierIcon(modifier)
    assert.notEqual(icon, undefined, `${modifier} has no icon`)
    assert.ok(icon!.paths.length > 0, `${modifier} has an empty glyph`)
    assert.ok(icon!.title.length > 0, `${modifier} has no title`)
  }
})

test('no modifier and an unknown one draw nothing rather than a wrong glyph', () => {
  assert.equal(modifierIcon(undefined), undefined)
  assert.equal(modifierIcon('none'), undefined)
  // A modifier from a newer runtime: no badge is honest, a borrowed glyph is not.
  assert.equal(modifierIcon('someLaterModifier'), undefined)
})

test('the path data is real SVG, not a placeholder', () => {
  for (const [name, icon] of Object.entries(MODIFIER_ICONS)) {
    for (const d of icon.paths) {
      // Every iconoir path starts with an absolute moveto and uses the 24-unit
      // grid the viewBox declares.
      assert.match(d, /^M[\d.]/, `${name} has a path that does not start with a moveto`)
    }
  }
})
