<script lang="ts">
  import type { ValueNode } from '../types/valueTree'
  import Self from './ValueTreeRow.svelte'

  let {
    node,
    depth = 0,
    expanded,
    ontoggle,
    oncopy,
  }: {
    node: ValueNode
    depth?: number
    expanded: Set<string>
    ontoggle: (id: string) => void
    oncopy: (text: string) => void
  } = $props()

  const open = $derived(expanded.has(node.id))
  const branch = $derived(node.children.length > 0)
</script>

<div class="row" class:branch style={`--depth: ${depth}`}>
  <button
    class="twist"
    type="button"
    disabled={!branch}
    aria-expanded={branch ? open : undefined}
    aria-label={branch ? `${open ? 'Collapse' : 'Expand'} ${node.label}` : undefined}
    onclick={() => branch && ontoggle(node.id)}
  >{branch ? (open ? '▾' : '▸') : ''}</button>
  <button class="name" type="button" title={`${node.label} / ${node.typeLabel}`} onclick={() => branch ? ontoggle(node.id) : oncopy(node.summary)}>{node.label}</button>
  <button class="value" type="button" title={node.summary ? `${node.summary}\n\nClick to copy` : undefined} onclick={() => oncopy(node.summary)}>{node.summary}</button>
</div>
{#if open}
  {#each node.children as child (child.id)}
    <Self node={child} depth={depth + 1} {expanded} {ontoggle} {oncopy} />
  {/each}
{/if}

<style>
  .row { display: grid; grid-template-columns: 16px minmax(70px, 118px) minmax(0, 1fr); align-items: center; min-height: 21px; padding-left: calc(var(--depth) * 12px); gap: 4px; }
  .row:hover { background: color-mix(in srgb, var(--node-color) 10%, transparent); }
  button { height: 19px; padding: 0 5px; border: 0; border-radius: 2px; background: transparent; font: 9px/1 ui-monospace, monospace; text-align: left; }
  .twist { padding: 0; color: var(--text-faint); font-size: 8px; text-align: center; }
  .twist:disabled { opacity: 0; }
  /* The name is a chip, as in the host's own object tree: it is a handle for
     the field, not prose. A branch is tinted so structure reads down the page. */
  .name { overflow: hidden; background: var(--surface-raised); color: var(--text-muted); text-overflow: ellipsis; white-space: nowrap; }
  .branch > .name { background: color-mix(in srgb, var(--node-color) 30%, var(--surface-raised)); color: var(--text); }
  .value { overflow: hidden; color: var(--text-faint); text-overflow: ellipsis; white-space: nowrap; }
  .value:hover:not(:empty) { color: var(--text); }
</style>
