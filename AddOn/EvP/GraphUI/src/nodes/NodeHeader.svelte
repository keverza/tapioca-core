<script lang="ts">
  import type { NodeResultRecord, NodeTypeSchema } from '../types'
  import NodeStatus from './NodeStatus.svelte'

  let { schema, name, result, onrename, onmenu }: { schema: NodeTypeSchema; name: string; result?: NodeResultRecord; onrename: (name: string) => void; onmenu: () => void } = $props()
  let editing = $state(false)
  let draft = $state('')

  function begin(): void { draft = name; editing = true }
  function commit(): void { onrename(draft); editing = false }
</script>

<header title={`${schema.label}\n${schema.category}\n${schema.description}`}>
  <span class="type-mark" aria-hidden="true"></span>
  {#if editing}
    <input class="nodrag" bind:value={draft} onblur={commit} onkeydown={(event) => event.key === 'Enter' && commit()} aria-label="Node name" />
  {:else}
    <button class="name nodrag" type="button" ondblclick={begin} title="Double-click to rename">{name}</button>
  {/if}
  <NodeStatus {result} />
  <button class="quick nodrag" type="button" onclick={onmenu}>Quick</button>
</header>

<style>
  header { display: grid; grid-template-columns: 10px minmax(0, 1fr) auto auto; align-items: center; min-height: 34px; padding: 0 5px 0 10px; border-bottom: 1px solid var(--border); background: var(--surface-raised); gap: 5px; }
  .type-mark { width: 7px; height: 7px; transform: rotate(45deg); border: 1px solid var(--node-color); background: color-mix(in srgb, var(--node-color) 22%, transparent); }
  .name { overflow: hidden; height: 28px; padding: 0; border: 0; background: transparent; color: var(--text); font-size: 12px; font-weight: 600; text-align: left; text-overflow: ellipsis; white-space: nowrap; }
  input { min-width: 0; height: 24px; padding: 0 5px; }
  .quick { height: 24px; padding: 0 6px; border-color: transparent; background: transparent; color: var(--text-faint); font-size: 8px; }
</style>
