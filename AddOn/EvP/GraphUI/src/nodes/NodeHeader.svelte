<script lang="ts">
  import { tick } from 'svelte'
  import type { NodeResultRecord, NodeTypeSchema } from '../types'
  import NodeStatus from './NodeStatus.svelte'

  import type { ComponentMessage } from './types/diagnostics'
  let { schema, name, result, messages = [], canBrowse = false, onrename, onbrowse }: { schema: NodeTypeSchema; name: string; result?: NodeResultRecord; messages?: ComponentMessage[]; canBrowse?: boolean; onrename: (name: string) => void; onbrowse?: () => void } = $props()
  let editing = $state(false)
  let draft = $state('')
  let field = $state<HTMLInputElement>()

  export function begin(): void {
    draft = name
    editing = true
    // `tick`, not a microtask: the field does not exist until Svelte has
    // flushed this state change, and focusing a field that is not there yet
    // leaves the caret in whatever had it before.
    void tick().then(() => field?.select())
  }
  function commit(): void { onrename(draft); editing = false }

  /**
   * Right-click renames. The title bar is otherwise a DRAG HANDLE now - it
   * carries no `nodrag`, so grabbing a node by its name moves it like grabbing
   * any other part of it does, which is what a person tries first.
   */
  function contextRename(event: MouseEvent): void {
    event.preventDefault()
    event.stopPropagation()
    begin()
  }
</script>

<header role="toolbar" tabindex="-1" aria-label="Node title" oncontextmenu={contextRename} onkeydown={(event) => { if (event.key === 'F2') { event.preventDefault(); begin() } }}>
  <span class="type-mark" aria-hidden="true"></span>
  {#if editing}
    <input class="nodrag" bind:this={field} bind:value={draft} onblur={commit} onkeydown={(event) => { if (event.key === 'Enter') commit(); if (event.key === 'Escape') editing = false }} aria-label="Node name" />
  {:else}
    <!-- A span, not a button: a button swallows the drag gesture and announces
         itself as an action, and renaming is not what a press here does. -->
    <span class="name">{name}</span>
  {/if}
  <NodeStatus {result} {messages} />
  <!--
    ⚠️ THE DATA BROWSER, ON THE NODE. It was reachable only through the
    middle-click ring, which means a user who has not found that gesture cannot
    see what their node produced at all - and the status word right beside this
    is exactly where somebody looks when they want to know what happened. An
    arrow, because it POPS OUT a panel to the side; that is what it does and what
    it looks like.

    Absent, not disabled, on a node with nothing to browse: a greyed button
    invites a click that will never work and says nothing about why.
  -->
  {#if canBrowse}
    <button class="browse nodrag" type="button" title="Browse this node's data" aria-label="Browse node data" onclick={onbrowse}>&rsaquo;</button>
  {/if}

  <!--
    The description, ABOVE the node rather than in a native tooltip: it is the
    one place the node's own type, category and gestures are written down, and a
    `title` attribute cannot say any of that legibly.
  -->
  <aside class="describe" aria-hidden="true">
    <strong>{schema.label}</strong>
    <span class="category">{schema.category}</span>
    <p>{schema.description}</p>
    <dl>
      <dt>Type</dt><dd>{schema.nodeType}</dd>
      <dt>Runs on</dt><dd>{schema.executionDomain}</dd>
    </dl>
    <em>Middle-click for quick actions / right-click the title to rename</em>
  </aside>
</header>

<style>
  header { position: relative; display: grid; grid-template-columns: 10px minmax(0, 1fr) auto auto; align-items: center; min-height: 34px; padding: 0 7px 0 10px; border-bottom: 1px solid var(--border); background: var(--surface-raised); gap: 5px; }
  .browse { width: 17px; height: 20px; padding: 0; border: 1px solid transparent; border-radius: 2px; background: transparent; color: var(--text-faint); font: 13px/1 'Segoe UI', sans-serif; cursor: pointer; }
  .browse:hover { border-color: var(--border); color: var(--text); }
  .type-mark { width: 7px; height: 7px; transform: rotate(45deg); border: 1px solid var(--node-color); background: color-mix(in srgb, var(--node-color) 22%, transparent); }
  .name { overflow: hidden; color: var(--text); font-size: 12px; font-weight: 600; text-overflow: ellipsis; white-space: nowrap; }
  input { min-width: 0; height: 24px; padding: 0 5px; }

  /* Positioned against the NODE (article is the positioned ancestor), so it
     sits above the whole component instead of over its own title bar. */
  .describe { position: absolute; z-index: 19; bottom: calc(100% + 9px); left: -1px; display: grid; visibility: hidden; box-sizing: border-box; width: max-content; max-width: 260px; padding: 9px 10px; border: 1px solid var(--border); border-top: 2px solid var(--node-color); border-radius: 3px; opacity: 0; background: var(--surface); box-shadow: 0 14px 34px rgb(0 0 0 / 46%); gap: 4px; pointer-events: none; text-align: left; transition: opacity .12s ease, visibility .12s; }
  header:hover .describe { visibility: visible; opacity: 1; transition-delay: .45s; }
  .describe strong { color: var(--text); font-size: 11px; }
  .describe .category { color: var(--node-color); font: 7px/1 ui-monospace, monospace; letter-spacing: .1em; text-transform: uppercase; }
  .describe p { margin: 2px 0 0; color: var(--text-muted); font-size: 9px; line-height: 1.45; }
  .describe dl { display: grid; margin: 4px 0 0; grid-template-columns: auto 1fr; gap: 2px 8px; }
  .describe dt { color: var(--text-faint); font: 7px/1.4 ui-monospace, monospace; }
  .describe dd { margin: 0; color: var(--text-muted); font: 7px/1.4 ui-monospace, monospace; }
  .describe em { margin-top: 5px; padding-top: 5px; border-top: 1px solid var(--border); color: var(--text-faint); font: 7px/1.4 ui-monospace, monospace; font-style: normal; }
</style>
