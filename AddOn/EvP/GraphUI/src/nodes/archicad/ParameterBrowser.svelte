<script lang="ts">
  import { SvelteSet } from 'svelte/reactivity'
  import type {
    ElementDescriptionResponse,
    ElementGroup,
    GraphParameter,
    NodeOutputRecord,
    NodeTypeSchema,
    SelectionAction,
    SettingMenuTarget,
  } from '../../types'
  import { filterValueTree, flattenValueTree, nodeValueTree } from '../types/valueTree'
  import PropertyList from './PropertyList.svelte'
  import ValueTreeRow from './ValueTreeRow.svelte'

  let {
    nodeId,
    count,
    parameters,
    outputs = [],
    outputPorts = [],
    busy = false,
    isSelectionSet = false,
    elementGroups = [],
    ondescribeelements,
    onpromote,
    onsettingmenu,
    promoted = [],
    onclose,
    onselectionaction,
    oncopyreference,
  }: {
    nodeId: string
    count: number
    parameters: GraphParameter[]
    outputs?: NodeOutputRecord[]
    outputPorts?: NodeTypeSchema['outputs']
    busy?: boolean
    isSelectionSet?: boolean
    /** The Archicad elements this node holds, when it holds any. */
    elementGroups?: ElementGroup[]
    ondescribeelements?: (guids: string[]) => Promise<ElementDescriptionResponse>
    onpromote?: (target: SettingMenuTarget) => void
    onsettingmenu?: (target: SettingMenuTarget) => void
    /** Setting ids already promoted on this node. */
    promoted?: string[]
    onclose: () => void
    onselectionaction?: (nodeId: string, action: SelectionAction) => void
    oncopyreference?: (nodeId: string, portId: string, direction: 'output') => void
  } = $props()

  /**
   * ⚠️ PROPERTIES FIRST, VALUES SECOND, AND THAT ORDER IS THE POINT. This panel
   * used to open on the value tree - a list of GUIDs, which is what the node
   * PRODUCED. Somebody opening a browser on a wall wants to know what a wall
   * HAS. The tree is still here, below, because the identities are real and
   * occasionally what you want; it is no longer the first and only answer.
   */
  let tab = $state<'properties' | 'values'>('properties')

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
  /**
   * The tree as text, exactly as it is on screen - filtered, and indented the
   * way the rows are.
   *
   * ⚠️ WHAT IS VISIBLE, NOT WHAT IS THERE. Copying the unfiltered tree from a
   * panel showing four rows would hand over four hundred, which is not what the
   * button appeared to offer. Tab-separated, so it lands in a spreadsheet or an
   * issue as a table rather than as a paragraph.
   */
  function copyAll(): void {
    copy(flattenValueTree(tree).join('\n'))
  }
  function copyReference(portId: string): void {
    oncopyreference?.(nodeId, portId, 'output')
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
      <!--
        ⚠️ NO "COPY REFERENCE" HERE ANY MORE. It copied `outputPorts[0]`, which
        on a node with several outputs is whichever one the catalog happened to
        list first - a button whose meaning depended on registration order and
        said nothing about which port it had taken. Every output row in the tree
        below now carries its own, naming the port it copies.
      -->
    </div>
  {/if}

  <!--
    Two tabs, not two panels. §4 asks for Information / Properties / Relations /
    Display / Comment; two of those exist today and inventing the other three as
    empty shells would be three promises the build cannot keep.
  -->
  <nav class="tabs" aria-label="Browser sections">
    <button type="button" class:active={tab === 'properties'} onclick={() => (tab = 'properties')}>Properties</button>
    <button type="button" class:active={tab === 'values'} onclick={() => (tab = 'values')}>Values</button>
  </nav>

  {#if tab === 'properties'}
    <section class="pane">
      <PropertyList
        groups={elementGroups}
        ondescribe={ondescribeelements}
        {onpromote}
        onmenu={onsettingmenu}
        {promoted}
      />
    </section>
  {:else}
    <label>
      <span>Filter</span>
      <input bind:value={query} placeholder="Name, type or value" />
    </label>

    <nav aria-label="Tree controls">
      <button type="button" onclick={expandAll}>Expand all</button>
      <button type="button" onclick={() => (expanded = new SvelteSet())}>Collapse all</button>
      <button type="button" disabled={tree.length === 0} title="Copy the rows shown, tab separated" onclick={copyAll}>Copy all</button>
    </nav>

    <section>
      {#each tree as root (root.id)}
        <ValueTreeRow node={root} {expanded} ontoggle={toggle} oncopy={copy} oncopyreference={copyReference} />
      {:else}
        <p>
          {query.trim() === ''
            ? 'This node holds no stored fields and has published no outputs yet. Evaluate the graph to browse what it produces.'
            : `Nothing under this node matches "${query}".`}
        </p>
      {/each}
    </section>
  {/if}
</aside>

<style>
  aside {
    position: absolute;
    z-index: 12;
    top: 0;
    left: calc(100% + 10px);
    display: grid;
    grid-template-rows: auto auto auto minmax(100px, 1fr);
    width: 360px;
    max-height: 480px;
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
  .reference-actions { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 5px; padding: 8px 9px 0; }
  label { display: grid; gap: 5px; padding: 8px 9px; }
  input { width: 100%; height: 27px; padding: 0 7px; }
  nav { display: flex; padding: 0 9px 6px; border-bottom: 1px solid var(--border); gap: 5px; }
  nav button { border: 0; border-radius: 0; background: transparent; color: var(--text-faint); }
  nav button:hover { color: var(--text); }
  section { min-height: 0; padding: 5px 8px 8px; overflow: auto; background: var(--canvas); }
  section p { margin: 0; padding: 15px 8px; color: var(--text-faint); font-size: 9px; line-height: 1.45; text-align: center; }
  .tabs { display: flex; padding: 0 9px; border-bottom: 1px solid var(--border); gap: 2px; }
  .tabs button { border: 0; border-bottom: 2px solid transparent; border-radius: 0; background: transparent; color: var(--text-faint); }
  .tabs button.active { border-bottom-color: var(--node-color); color: var(--text); }
  .tabs button:hover { color: var(--text); }
  .pane { padding: 8px 9px 10px; }
  .pane :global(p) { padding: 0; text-align: left; }
</style>
