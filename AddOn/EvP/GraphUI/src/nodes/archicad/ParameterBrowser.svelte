<script lang="ts">
  import type { GraphParameter, SelectionAction } from '../../types'
  import PropertyReference from './PropertyReference.svelte'

  let {
    nodeId,
    count,
    parameters,
    busy = false,
    onclose,
    onselectionaction,
  }: {
    nodeId: string
    count: number
    parameters: GraphParameter[]
    busy?: boolean
    onclose: () => void
    onselectionaction?: (nodeId: string, action: SelectionAction) => void
  } = $props()

  let query = $state('')
  const rows = $derived(
    parameters
      .map((parameter) => ({
        name: parameter.parameterId,
        type: parameter.value?.valueType ?? 'absent',
        value: parameter.value?.text ?? parameter.value?.number?.toString() ?? `${parameter.value?.itemCount ?? 0} items`,
      }))
      .filter((row) => `${row.name} ${row.type}`.toLocaleLowerCase().includes(query.trim().toLocaleLowerCase())),
  )
</script>

<aside class="bim-browser nodrag nowheel" aria-label="BIM reference browser">
  <header>
    <div><span>Archicad reference</span><strong>{count} selected element{count === 1 ? '' : 's'}</strong></div>
    <button type="button" onclick={onclose} aria-label="Close property browser">Close</button>
  </header>

  <div class="reference-actions">
    <button type="button" disabled={busy || onselectionaction === undefined} onclick={() => onselectionaction?.(nodeId, 'update')}>Replace selection</button>
    <button type="button" disabled={busy || onselectionaction === undefined} onclick={() => onselectionaction?.(nodeId, 'reselect')}>Show in Archicad</button>
  </div>

  <label>
    <span>Filter stored fields</span>
    <input bind:value={query} placeholder="Name or type" />
  </label>

  <nav aria-label="Property groups">
    <button type="button" class="active">Stored data</button>
    <button type="button" disabled title="Provided by a future native property selector">Properties</button>
    <button type="button" disabled title="Provided by a future native property selector">Classification</button>
    <button type="button" disabled title="Provided by a future native property selector">Geometry</button>
  </nav>

  <section>
    {#each rows as row}
      <PropertyReference name={row.name} value={row.value} type={row.type} />
    {:else}
      <p>No stored node fields match. Full project properties remain native-owned.</p>
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
    width: 310px;
    max-height: 390px;
    overflow: hidden;
    border: 1px solid var(--border);
    border-top: 2px solid var(--node-color);
    border-radius: 2px;
    background: var(--surface);
    box-shadow: 0 18px 44px rgb(0 0 0 / 48%);
    color: var(--text);
  }
  header { display: flex; align-items: center; justify-content: space-between; padding: 10px; border-bottom: 1px solid var(--border); background: var(--surface-raised); }
  header div { display: grid; gap: 3px; }
  header span, label span { color: var(--text-faint); font: 8px/1 ui-monospace, monospace; letter-spacing: .1em; text-transform: uppercase; }
  header strong { font-size: 12px; font-weight: 550; }
  button { height: 25px; padding: 0 7px; font-size: 9px; }
  .reference-actions { display: flex; gap: 5px; padding: 8px 9px 0; }
  label { display: grid; gap: 5px; padding: 8px 9px; }
  input { width: 100%; height: 27px; padding: 0 7px; }
  nav { display: flex; padding: 0 9px; border-bottom: 1px solid var(--border); }
  nav button { border: 0; border-radius: 0; background: transparent; color: var(--text-faint); }
  nav button.active { border-bottom: 2px solid var(--node-color); color: var(--text); }
  section { min-height: 0; padding: 5px 8px 8px; overflow: auto; background: var(--canvas); }
  section p { margin: 0; padding: 15px 8px; color: var(--text-faint); font-size: 9px; line-height: 1.45; text-align: center; }
</style>
