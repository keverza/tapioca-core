/**
 * Close-on-outside-press, as one action rather than as a listener per panel.
 *
 * `pointerdown` on the window rather than `click` on the document: a menu that
 * waits for the click has already let the press through to the canvas
 * underneath, which is what made a node menu close AND drag the node in the
 * same gesture. Capture phase, so a panel that stops propagation on its own
 * subtree cannot stop this from seeing the press either.
 */
export function closeOnOutsidePress(element: HTMLElement, onoutside: () => void) {
  let handler = onoutside

  function press(event: PointerEvent): void {
    const target = event.target
    if (!(target instanceof Node)) return
    if (element.contains(target)) return
    // The control that OPENS a panel is outside it, and it usually toggles.
    // Without this, pressing it while the panel is open closes the panel on the
    // press and the click immediately reopens it - a toggle that never toggles.
    // The control marks itself instead of every panel learning about it.
    if (target instanceof Element && target.closest('[data-menu-toggle]') !== null) return
    handler()
  }

  // Deferred a frame: the very press that OPENED the panel is still being
  // dispatched, and a listener added during it would see that same press and
  // close the panel it just opened.
  const armed = requestAnimationFrame(() =>
    window.addEventListener('pointerdown', press, { capture: true }),
  )

  return {
    update(next: () => void) {
      handler = next
    },
    destroy() {
      cancelAnimationFrame(armed)
      window.removeEventListener('pointerdown', press, { capture: true })
    },
  }
}
