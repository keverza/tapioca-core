<script lang="ts">
  import { SvelteSet } from 'svelte/reactivity'
  import type { GraphParameter, NodeOutputRecord, SelectionAction } from '../../types'
  import { filterValueTree, nodeValueTree } from '../types/valueTree'
  import ValueTreeRow from './ValueTreeRow.svelte'

  let {
    nodeId,
    count,
    parameters,
    outputs = [],
    busy = false,
    isSelectionSet = false,
    onclose,
    onselectionaction,
  }: {
    nodeId: string
    count: number
    parameters: GraphParameter[]
    outputs?: NodeOutputRecord[]
    busy?: boolean
    isSelectionSet?: boolean
    onclose: () => void
    onselectionaction?: (nodeId: string, action: SelectionAction) => void
  } = $props()

  let query = $state('')
  // Roots open, leaves closed: the shape is what you want first, and a
  // thousand-element list expanded on open is not a browser.
  let expanded = $state(new SvelteSet(['stored', 'outputs']))

  const tree = $derived(filterValueTree(nodeValueTree(parameters, outputs), query))

  function toggle(id: string): void {
    if (expanded.has(id)) expanded.delete(id)
    else expanded.add(id)
  }
  function copy(text: string): void {
    if (text !== '') void navigator.clipboard?.writeText(text)
  }
  function expandAll(): void {
    const ids: string[] = []
    const walk = (nodes: typeof tree) => {
      for (const node of nodes) {
        if (node.children.length > 0) ids.push(node.id)
        walk(node.children)
      }
    }
    walk(tree)
    expanded = new SvelteSet(ids)
  }
</script>

<aside class="bim-browser nodrag nowheel" aria-label="Node data browser">
  <header>
    <div>
      <span>{isSelectionSet ? 'Archicad reference' : 'Node data'}</span>
      <strong>{isSelectionSet ? `${count} selected element${count === 1 ? '' : 's'}` : nodeId}</strong>
    </div>
    <button type="button" onclick={onclose} aria-label="Close data browser">Close</button>
  </header>

  {#if isSelectionSet}
    <div class="reference-actions">
      <button type="button" disabled={busy || onselectionaction === undefined} onclick={() => onselectionaction?.(nodeId, 'update')}>Replace selection</button>
      <button type="button" disabled={busy || onselectionaction === undefined} onclick={() => onselectionaction?.(nodeId, 'reselect')}>Show in Archicad</button>
    </div>
  {/if}

  <label>
    <span>Filter</span>
    <input bind:value={query} placeholder="Name, type or value" />
  </label>

  <nav aria-label="Tree controls">
    <button type="button" onclick={expandAll}>Expand all</button>
    <button type="button" onclick={() => (expanded = new SvelteSet())}>Collapse all</button>
  </nav>

  <section>
    {#each tree as root (root.id)}
      <ValueTreeRow node={root} {expanded} ontoggle={toggle} oncopy={copy} />
    {:else}
      <p>
        {query.trim() === ''
          ? 'This node holds no stored fields and has published no outputs yet. Evaluate the graph to browse what it produces.'
          : `Nothing under this node matches "${query}".`}
      </p>
    {/each}
  </section>
</aside>

<style>
  aside {
    position: absolute;
    z-index: 12;
    top: 0;
    left: calc(100% + 10px);
    display: grid;
    grid-template-rows: auto auto auto auto minmax(100px, 1fr);
    width: 330px;
    max-height: 420px;
    overflow: hidden;
    border: 1px solid var(--border);
    border-top: 2px solid var(--node-color);
    border-radius: 2px;
    background: var(--surface);
    box-shadow: 0 18px 44px rgb(0 0 0 / 48%);
    color: var(--text);
  }
  header { display: flex; align-items: center; justify-content: space-between; padding: 10px; border-bottom: 1px solid var(--border); background: var(--surface-raised); }
  header div { display: grid; min-width: 0; gap: 3px; }
  header span, label span { color: var(--text-faint); font: 8px/1 ui-monospace, monospace; letter-spacing: .1em; text-transform: uppercase; }
  header strong { overflow: hidden; font-size: 12px; font-weight: 550; text-overflow: ellipsis; white-space: nowrap; }
  button { height: 25px; padding: 0 7px; font-size: 9px; }
  .reference-actions { display: flex; gap: 5px; padding: 8px 9px 0; }
  label { display: grid; gap: 5px; padding: 8px 9px; }
  input { width: 100%; height: 27px; padding: 0 7px; }
  nav { display: flex; padding: 0 9px 6px; border-bottom: 1px solid var(--border); gap: 5px; }
  nav button { border: 0; border-radius: 0; background: transparent; color: var(--text-faint); }
  nav button:hover { color: var(--text); }
  section { min-height: 0; padding: 5px 8px 8px; overflow: auto; background: var(--canvas); }
  section p { margin: 0; padding: 15px 8px; color: var(--text-faint); font-size: 9px; line-height: 1.45; text-align: center; }
</style>
