/**
 * Iconoir glyph paths, inlined.
 *
 * ⚠️ INLINE PATH DATA, NOT FILES, AND NOT BY PREFERENCE. `npm run build` asserts
 * the bundle is ONE index.html with no external script, style, localhost or HTTP
 * reference - the page is delivered as a DATA resource inside the .apx and there
 * is no server to fetch anything from. An <img src="folder.svg"> would build
 * cleanly and render as a broken image inside Archicad.
 *
 * ⚠️ AND THEY ARE COPIED, NOT IMPORTED FROM AddOn/EvP/Icons/. Those files are
 * compiled into the NATIVE resource bundle for the DG palette; this is a browser
 * bundle built by Vite from a different tree. Reaching across would couple the
 * two build graphs for two glyphs. The upstream source is the same, so a glyph
 * that changes there should be re-copied here - which is why each one names its
 * file.
 *
 * Iconoir is MIT and already carries its required notice in the repository
 * NOTICE, under the entry for the palette controls. These come from the same
 * release; adding a glyph here needs no new notice.
 *
 * Every path is drawn on a 24x24 viewBox with `stroke="currentColor"` and no
 * fill, so a button's own colour drives them and a disabled button greys its
 * icon with no extra rule.
 */

export interface IconGlyph {
  /** The upstream file, so a glyph can be re-copied when Iconoir changes. */
  readonly source: string
  readonly paths: readonly string[]
}

/** iconoir/icons/regular/folder.svg */
export const FOLDER: IconGlyph = {
  source: 'iconoir/icons/regular/folder.svg',
  paths: [
    'M2 11V4.6C2 4.26863 2.26863 4 2.6 4H8.77805C8.92127 4 9.05977 4.05124 9.16852 4.14445L12.3315 6.85555C12.4402 6.94876 12.5787 7 12.722 7H21.4C21.7314 7 22 7.26863 22 7.6V11M2 11V19.4C2 19.7314 2.26863 20 2.6 20H21.4C21.7314 20 22 19.7314 22 19.4V11M2 11H22',
  ],
}
