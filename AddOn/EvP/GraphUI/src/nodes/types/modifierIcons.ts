/**
 * The glyph drawn on a port that carries a modifier.
 *
 * ⚠️ A MODIFIER YOU CANNOT SEE IS A HIDDEN NODE. That is the whole objection to
 * port modifiers, and a badge on the port is the answer to it: reading a graph
 * has to show what each port does to what arrives, or the canvas stops being a
 * complete description of what runs. The context menu ticks the one in force,
 * but nobody opens six menus to read a node.
 *
 * ⚠️ THE PATH DATA IS EMBEDDED, NOT IMPORTED. Iconoir lives in `tapioca-ref`,
 * which is outside both repositories and is never published; a build that read
 * SVGs from there would work on this machine and nowhere else. These are the
 * exact `d` attributes from iconoir's regular set, copied in with attribution.
 *
 * Iconoir (https://iconoir.com) - MIT, Copyright (c) 2021 Luca Burgio.
 * All drawn on a 24x24 viewBox with `stroke-width: 1.5` and round caps, which is
 * what makes them legible at the ~11px a port badge gets.
 */
export interface ModifierIcon {
  /** The `d` attributes of every stroke, in draw order. */
  paths: string[]
  /** What the modifier does, for the badge's title and for a screen reader. */
  title: string
}

export const MODIFIER_ICONS: Record<string, ModifierIcon> = {
  // arrow-union-vertical: two chevrons closing inward - many branches into one.
  flatten: {
    paths: ['M17 4L12 9L7 4', 'M17 20L12 15L7 20'],
    title: 'Flatten: every branch collapsed into one list',
  },
  // arrow-separate-vertical: two chevrons opening outward - one becomes many.
  graft: {
    paths: ['M17 8L12 3L7 8', 'M17 16L12 21L7 16'],
    title: 'Graft: every item gets its own branch',
  },
  // collapse: four corners drawn inward - the shared path prefix dropped.
  simplify: {
    paths: [
      'M20 20L15 15M15 15V19M15 15H19',
      'M4 20L9 15M9 15V19M9 15H5',
      'M20 4L15 9M15 9V5M15 9H19',
      'M4 4L9 9M9 9V5M9 9H5',
    ],
    title: 'Simplify: the path prefix every branch shares is dropped',
  },
  // sort: descending bars beside an up/down arrow - the order turned around.
  reverse: {
    paths: ['M10 14H2', 'M8 10H2', 'M6 6H2', 'M12 18H2', 'M19 20V4M19 20L22 17M19 20L16 17M19 4L22 7M19 4L16 7'],
    title: 'Reverse: each branch in the opposite order',
  },
  // hashtag: the number sign - a value made whole.
  round: {
    paths: ['M10 3L6 21', 'M20.5 16H2.5', 'M22 7H4', 'M18 3L14 21'],
    title: 'Round: numbers converted to whole numbers, to nearest',
  },
  // ruler-arrows: a measured span with range arrows - a range remapped.
  normalise: {
    paths: [
      'M15.4 22H8.6C8.26863 22 8 21.7314 8 21.4V2.6C8 2.26863 8.26863 2 8.6 2H15.4C15.7314 2 16 2.26863 16 2.6V21.4C16 21.7314 15.7314 22 15.4 22Z',
      'M16 17H13',
      'M16 7H13',
      'M13 12H23M23 12L21 14M23 12L21 10',
      'M1 12L3 10M1 12L3 14M1 12H8',
    ],
    title: 'Normalise: each branch remapped onto 0 to 1',
  },
}

/** The icon for a modifier, or undefined for `none` and anything unknown. */
export function modifierIcon(modifier: string | undefined): ModifierIcon | undefined {
  if (modifier === undefined || modifier === 'none') return undefined
  return MODIFIER_ICONS[modifier]
}
