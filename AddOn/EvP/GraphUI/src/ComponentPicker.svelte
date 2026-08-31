<script lang="ts">
  import { filterCatalog, groupCatalog, NODE_DRAG_MIME } from './editor'
  import type { NodeTypeSchema } from './types'

  let {
    catalog,
    open,
    busy,
    onopen,
    onclose,
    onplace,
  }: {
    catalog: NodeTypeSchema[]
    open: boolean
    busy: boolean
    onopen: () => void
    onclose: () => void
    onplace: (nodeType: string) => void
  } = $props()

  let query = $state('')
  let activeIndex = $state(0)
  let searchInput = $state<HTMLInputElement>()

  const filtered = $derived(filterCatalog(catalog, query))
  const groups = $derived(groupCatalog(filtered))

  $effect(() => {
    query
    activeIndex = 0
  })

  $effect(() => {
    if (open) queueMicrotask(() => searchInput?.focus())
  })

  function startDrag(event: DragEvent, nodeType: string): void {
    if (event.dataTransfer === null) return
    event.dataTransfer.setData(NODE_DRAG_MIME, nodeType)
    event.dataTransfer.effectAllowed = 'move'
  }

  function handleSearchKey(event: KeyboardEvent): void {
    if (event.key === 'ArrowDown') {
      activeIndex = Math.min(activeIndex + 1, filtered.length - 1)
      event.preventDefault()
    } else if (event.key === 'ArrowUp') {
      activeIndex = Math.max(activeIndex - 1, 0)
      event.preventDefault()
    } else if (event.key === 'Enter' && filtered[activeIndex] !== undefined) {
      onplace(filtered[activeIndex].nodeType)
      event.preventDefault()
    } else if (event.key === 'Escape') {
      onclose()
      event.preventDefault()
    }
  }
</script>

{#if open}
  <aside class="component-picker nodrag nowheel" aria-label="Component picker">
    <header>
      <div>
        <span>Native catalog</span>
        <strong>Components</strong>
      </div>
      <button class="icon-button" type="button" onclick={onclose} aria-label="Collapse component picker">&#x2039;</button>
    </header>
    <label class="catalog-search">
      <span class="sr-only">Search components</span>
      <input
        bind:this={searchInput}
        bind:value={query}
        onkeydown={handleSearchKey}
        type="search"
        placeholder="Search nodes and ports"
        autocomplete="off"
      />
      {#if query !== ''}
        <button type="button" onclick={() => (query = '')} aria-label="Clear component search">Clear</button>
      {/if}
    </label>
    <div class="catalog-results" aria-live="polite">{filtered.length} of {catalog.length} components</div>
    <div class="catalog-groups">
      {#each groups as [category, items]}
        <section>
          <h2>{category}</h2>
          {#each items as item}
            {@const itemIndex = filtered.indexOf(item)}
            <button
              type="button"
              class:active={itemIndex === activeIndex}
              disabled={busy}
              draggable={!busy}
              ondragstart={(event) => startDrag(event, item.nodeType)}
              onpointerenter={() => (activeIndex = itemIndex)}
              onclick={() => onplace(item.nodeType)}
            >
              <strong>{item.label}</strong>
              <span>{item.description}</span>
              <small>{item.inputs.length} in / {item.outputs.length} out</small>
            </button>
          {/each}
        </section>
      {:else}
        <p class="catalog-empty">No component matches every search term.</p>
      {/each}
    </div>
  </aside>
{:else}
  <button class="component-rail nodrag" type="button" onclick={onopen} aria-label="Open component picker">
    <span>+</span>
    <strong>Components</strong>
  </button>
{/if}
