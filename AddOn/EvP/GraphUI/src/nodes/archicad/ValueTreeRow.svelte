<script lang="ts">
  import type { ValueNode } from '../types/valueTree'
  import Self from './ValueTreeRow.svelte'

  let {
    node,
    depth = 0,
    expanded,
    ontoggle,
    oncopy,
    oncopyreference,
  }: {
    node: ValueNode
    depth?: number
    expanded: Set<string>
    ontoggle: (id: string) => void
    oncopy: (text: string) => void
    oncopyreference?: (portId: string) => void
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
  <!--
    ⚠️ ON THE PORT ROWS ONLY, and that is a correctness rule rather than a
    tidiness one. A port reference addresses `node.port`; a Copy button on a
    `[3]` row would hand over a reference to the WHOLE list while appearing to
    name one item, and the wire it produced would carry forty walls where the
    button said one. Rows that cannot be referenced carry no button at all
    rather than a disabled one, because there is nothing here the user could do
    differently.
  -->
  {#if node.portId !== undefined && oncopyreference !== undefined}
    <button
      class="reference"
      type="button"
      title={`Copy a reference to this output - paste it onto another node's input to wire it`}
      aria-label={`Copy a reference to ${node.label}`}
      onclick={() => oncopyreference(node.portId!)}
    >ref</button>
  {:else}
    <span class="reference-gap"></span>
  {/if}
</div>
{#if open}
  {#each node.children as child (child.id)}
    <Self node={child} depth={depth + 1} {expanded} {ontoggle} {oncopy} {oncopyreference} />
  {/each}
{/if}

<style>
  .row { display: grid; grid-template-columns: 16px minmax(70px, 118px) minmax(0, 1fr) 26px; align-items: center; min-height: 21px; padding-left: calc(var(--depth) * 12px); gap: 4px; }
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
  .reference { padding: 0 4px; border: 1px solid var(--border); color: var(--text-faint); font-size: 8px; text-align: center; cursor: pointer; }
  .reference:hover { border-color: var(--node-color); color: var(--text); }
  .reference-gap { display: block; }
</style>
