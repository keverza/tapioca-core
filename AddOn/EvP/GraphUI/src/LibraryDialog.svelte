<script lang="ts">
  import { onMount } from 'svelte'
  import { describeNameRule, isValidGraphName } from './editor'
  import type { StoredGraphInfo } from './library'

  export type LibraryMode = 'load' | 'save'

  let {
    mode,
    graphs,
    location,
    busy,
    error,
    suggestedName,
    onconfirm,
    ondelete,
    oncancel,
  }: {
    mode: LibraryMode
    graphs: StoredGraphInfo[]
    location: string
    busy: boolean
    error: string
    suggestedName: string
    onconfirm: (name: string) => void
    ondelete: (name: string) => void
    oncancel: () => void
  } = $props()

  // Seeded in onMount rather than from the prop directly: the dialog is created
  // fresh on every open, so the suggestion is a starting value the user then
  // owns, not something that should track the prop afterwards.
  let name = $state('')
  let field = $state<HTMLInputElement>()
  let list = $state<HTMLElement>()

  // A save over an existing name replaces it, so the button says so rather than
  // letting the user discover it afterwards.
  const overwriting = $derived(mode === 'save' && graphs.some((graph) => graph.name === name))
  const nameOk = $derived(isValidGraphName(name))
  const canConfirm = $derived(!busy && nameOk)

  onMount(() => {
    name = suggestedName
    if (mode === 'save') queueMicrotask(() => field?.select())
    else queueMicrotask(() => list?.querySelector<HTMLElement>('button')?.focus())
  })

  function confirm(): void {
    if (!canConfirm) return
    onconfirm(name)
  }

  function keydown(event: KeyboardEvent): void {
    if (event.key === 'Escape') {
      oncancel()
      event.preventDefault()
    } else if (event.key === 'Enter' && event.target === field) {
      confirm()
      event.preventDefault()
    }
  }
</script>

<!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
<div class="dialog-scrim" role="presentation" onclick={oncancel}></div>
<div
  class="dialog nodrag nowheel"
  role="dialog"
  tabindex="-1"
  aria-modal="true"
  aria-label={mode === 'save' ? 'Save graph to the workflow library' : 'Load graph from the workflow library'}
  onkeydown={keydown}
>
  <header>
    <strong>{mode === 'save' ? 'Save graph' : 'Load graph'}</strong>
    <span title={location}>{location === '' ? 'no library location on this machine' : location}</span>
  </header>

  {#if mode === 'save'}
    <label class="dialog-field">
      Name
      <input bind:this={field} bind:value={name} spellcheck="false" autocomplete="off" disabled={busy} />
    </label>
    {#if name.length > 0 && !nameOk}
      <p class="dialog-note danger">{describeNameRule()}</p>
    {:else if overwriting}
      <p class="dialog-note">A graph called <strong>{name}</strong> exists and will be replaced.</p>
    {/if}
  {/if}

  <div class="dialog-list" bind:this={list} role="listbox" aria-label="Saved graphs">
    {#if graphs.length === 0}
      <p class="dialog-empty">
        {mode === 'save'
          ? 'The library is empty. This will be the first graph in it.'
          : 'The library is empty. Save a graph first.'}
      </p>
    {:else}
      {#each graphs as graph}
        <div class="dialog-row" class:selected={graph.name === name}>
          <button
            type="button"
            role="option"
            aria-selected={graph.name === name}
            disabled={busy}
            onclick={() => {
              name = graph.name
              if (mode === 'load') confirm()
            }}
            ondblclick={() => {
              name = graph.name
              confirm()
            }}
          >
            <strong>{graph.label === '' ? graph.name : graph.label}</strong>
            <span>{graph.name} / {graph.nodeCount} node{graph.nodeCount === 1 ? '' : 's'}</span>
          </button>
          <button
            class="dialog-delete"
            type="button"
            title={`Delete ${graph.name} from the library`}
            aria-label={`Delete ${graph.name}`}
            disabled={busy}
            onclick={() => ondelete(graph.name)}
          >&times;</button>
        </div>
      {/each}
    {/if}
  </div>

  {#if error !== ''}
    <p class="dialog-note danger" role="alert">{error}</p>
  {/if}

  <footer>
    <button type="button" onclick={oncancel} disabled={busy}>Cancel</button>
    <button class="primary" type="button" onclick={confirm} disabled={!canConfirm}>
      {mode === 'save' ? (overwriting ? 'Replace' : 'Save') : 'Load'}
    </button>
  </footer>
</div>
