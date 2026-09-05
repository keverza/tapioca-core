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

/**
 * iconoir/icons/regular/reload-window.svg — reload this node from its file.
 *
 * A window with a refresh arrow rather than the bare `refresh.svg` circle: the
 * bare one is what a browser's reload means and reads as "run it again", which
 * is the one thing this button does NOT do. The graph evaluates continuously;
 * this re-READS the folder and reshapes the node's ports.
 */
export const RELOAD_WINDOW: IconGlyph = {
  source: 'iconoir/icons/regular/reload-window.svg',
  paths: [
    'M11 21H4C2.89543 21 2 20.1046 2 19V5C2 3.89543 2.89543 3 4 3H20C21.1046 3 22 3.89543 22 5V12',
    'M2 7L22 7',
    'M5 5.01L5.01 4.99889',
    'M8 5.01L8.01 4.99889',
    'M11 5.01L11.01 4.99889',
    'M21.6665 16.6667C21.0477 15.097 19.6346 14 17.9903 14C16.2319 14 14.7378 15.2545 14.1969 17',
    'M19.9952 16.7723H21.4002C21.7316 16.7723 22.0002 16.5036 22.0002 16.1723V14.55',
    'M14.3337 19.3333C14.9525 20.903 16.3657 22 18.01 22C19.7684 22 21.2624 20.7455 21.8033 19',
    'M16.005 19.2277H14.6001C14.2687 19.2277 14.0001 19.4964 14.0001 19.8277V21.45',
  ],
}

/**
 * iconoir/icons/regular/page-plus.svg — scaffold this node's folder.
 *
 * A PAGE with a plus, not a folder with one: what the press brings into
 * existence that the user then looks at is `main.py`. The folder around it is
 * bookkeeping.
 */
export const PAGE_PLUS: IconGlyph = {
  source: 'iconoir/icons/regular/page-plus.svg',
  paths: [
    'M4 12V2.6C4 2.26863 4.26863 2 4.6 2H16.2515C16.4106 2 16.5632 2.06321 16.6757 2.17574L19.8243 5.32426C19.9368 5.43679 20 5.5894 20 5.74853V21.4C20 21.7314 19.7314 22 19.4 22H11',
    'M16 2V5.4C16 5.73137 16.2686 6 16.6 6H20',
    'M1.99219 19H4.99219M7.99219 19H4.99219M4.99219 19V16M4.99219 19V22',
  ],
}

/** iconoir/icons/regular/code-brackets.svg — open the Script Inspector. */
export const CODE_BRACKETS: IconGlyph = {
  source: 'iconoir/icons/regular/code-brackets.svg',
  paths: [
    'M9.00001 21L8.00001 21C6.89544 21 6.00001 20.1057 6.00001 19.0011C6.00001 17.4501 6.00001 15.3443 6 14C6 13 4.5 12 4.5 12C4.5 12 6.00001 11 6.00001 10C6.00001 8.827 6.00001 6.62207 6.00001 4.99914C6.00001 3.89457 6.89544 3 8.00001 3L9.00001 3',
    'M15 21L16 21C17.1046 21 18 20.1057 18 19.0011C18 17.4501 18 15.3443 18 14C18 13 19.5 12 19.5 12C19.5 12 18 11 18 10C18 8.827 18 6.62207 18 4.99914C18 3.89457 17.1046 3 16 3L15 3',
  ],
}
