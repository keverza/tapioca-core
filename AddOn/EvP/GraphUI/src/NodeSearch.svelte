<script lang="ts">
  import { closeOnOutsidePress } from './interaction'
  import { filterCatalog } from './editor'
  import type { NodeTypeSchema } from './types'

  let {
    catalog,
    x,
    y,
    onplace,
    onclose,
  }: {
    catalog: NodeTypeSchema[]
    x: number
    y: number
    onplace: (nodeType: string) => void
    onclose: () => void
  } = $props()

  let query = $state('')
  let activeIndex = $state(0)
  let field = $state<HTMLInputElement>()

  // Bounded: this is a drop-in-place palette, not the catalog browser. The rail
  // is still there for reading the whole catalog.
  const matches = $derived((query.trim() === '' ? catalog : filterCatalog(catalog, query)).slice(0, 8))

  $effect(() => {
    query
    activeIndex = 0
  })
  $effect(() => {
    queueMicrotask(() => field?.focus())
  })

  function place(nodeType: string): void {
    onplace(nodeType)
    onclose()
  }

  function key(event: KeyboardEvent): void {
    if (event.key === 'ArrowDown') {
      activeIndex = Math.min(activeIndex + 1, matches.length - 1)
    } else if (event.key === 'ArrowUp') {
      activeIndex = Math.max(activeIndex - 1, 0)
    } else if (event.key === 'Enter' && matches[activeIndex] !== undefined) {
      place(matches[activeIndex].nodeType)
    } else if (event.key === 'Escape') {
      onclose()
    } else {
      return
    }
    event.preventDefault()
    event.stopPropagation()
  }
</script>

<div class="node-search nodrag nowheel" style={`left:${x}px; top:${y}px`} use:closeOnOutsidePress={onclose}>
  <input
    bind:this={field}
    bind:value={query}
    onkeydown={key}
    placeholder="Search components"
    aria-label="Search components"
  />
  {#if matches.length === 0}
    <p>No component matches "{query}".</p>
  {:else}
    <ul role="listbox" aria-label="Matching components">
      {#each matches as match, index}
        <li>
          <button
            type="button"
            role="option"
            aria-selected={index === activeIndex}
            class:active={index === activeIndex}
            onclick={() => place(match.nodeType)}
          >
            <strong>{match.label}</strong><small>{match.category}</small>
          </button>
        </li>
      {/each}
    </ul>
  {/if}
</div>

<style>
  .node-search { position: absolute; z-index: 22; width: 244px; padding: 6px; border: 1px solid var(--border); border-radius: 4px; background: var(--surface); box-shadow: 0 18px 44px rgb(0 0 0 / 50%); }
  input { width: 100%; height: 28px; padding: 0 8px; box-sizing: border-box; }
  ul { display: grid; margin: 5px 0 0; padding: 0; list-style: none; }
  button { display: grid; width: 100%; height: 26px; grid-template-columns: 1fr auto; align-items: center; padding: 0 7px; border: 0; border-radius: 2px; background: transparent; gap: 8px; text-align: left; }
  button.active, button:hover { background: var(--surface-hover); }
  strong { overflow: hidden; color: var(--text); font-size: 10px; font-weight: 500; text-overflow: ellipsis; white-space: nowrap; }
  small { color: var(--text-faint); font: 7px/1 ui-monospace, monospace; }
  p { margin: 8px 6px 4px; color: var(--text-faint); font-size: 9px; }
</style>
