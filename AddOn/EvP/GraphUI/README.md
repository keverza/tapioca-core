# GraphUI

GraphUI follows the canonical patterns in the pinned `@xyflow/svelte` examples from the
`xyflow/xyflow` repository. New graph interactions extend these seams instead of creating
an alternate canvas abstraction.

## Canonical structure

- `App.svelte` owns only `SvelteFlowProvider` and mounts `GraphEditor`.
- `GraphEditor.svelte` owns the controlled `$state.raw` node and edge arrays and the
  native graph reconciliation boundary.
- Components such as `ComponentPicker`, `ApplicationMenu`, and `ContextMenu` own isolated
  presentation and interaction state. They do not call the native bridge directly.
- `editor.ts` contains pure catalog, validation, viewport, and preference logic. Every new
  pure rule gets a Node test in `tests/`.
- `SchemaNode.svelte` renders catalog schema. Frontend code must not branch on a concrete
  native node type.
- Shared colors and detail behavior use semantic tokens in `styles.css`; feature
  components do not establish separate themes.

## Svelte Flow rules

Use the library's public example seams directly:

- Wrap consumers of `useSvelteFlow` in `SvelteFlowProvider`.
- Convert drops with `screenToFlowPosition`; do not reproduce viewport transform math.
- Pass one top-level `isValidConnection` function for immediate connection affordance.
- Keep `nodes` and `edges` controlled with `bind:nodes` and `bind:edges`.
- Use `onmove` and the public viewport shape for contextual detail.
- Use `colorMode` and `colorModeSSR` for the canvas, with application tokens for chrome.
- Use Svelte Flow node, edge, and pane events for contextual actions.
- Render presentation-only rectangles and visual frames through `ViewportPortal target="back"`;
  they belong to versioned editor metadata and never enter the semantic node array.
- Pointer-path tools may collect browser hit previews, but eraser and scissors deletion must
  submit one `Tapioca.GraphEraseElements` command on pointer-up.

Tapioca has one deliberate difference from the standalone examples: C++ owns semantic
graph state. `onbeforeconnect` always returns `false`, submits the candidate to the native
command, and installs only the canonical state returned by C++. Drop, delete, duplicate,
reparent, and future semantic tools must preserve the same rule. Browser-only overlays may
preview intent but may not become authoritative semantic objects.

## Adding an example-derived feature

1. Read the example matching the pinned `@xyflow/svelte` version.
2. Reuse its public provider, hook, prop, and event pattern.
3. Put reusable pure policy in `editor.ts` and add a deterministic test.
4. Keep bridge calls in `GraphEditor.svelte` until a reusable native command service is
   justified.
5. Run `npm test`, `npm run typecheck`, and `npm run build`.
