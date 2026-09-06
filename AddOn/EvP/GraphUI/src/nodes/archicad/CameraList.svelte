<script lang="ts">
  import type { StoredCamera } from './cameras'
  import { formatPosition, formatSun } from './cameras'

  /**
   * The camera node's body: the list, and the four things you can do to it.
   *
   * ⚠️ RESTORE AND REMOVE ARE PER ROW, ADD AND CLEAR ARE NOT, and that split is
   * the whole reason this is not the selection set's button strip. Restore and
   * Remove both need to name WHICH camera, and a strip of five buttons across
   * the bottom would have needed a separate "selected row" the user had to set
   * first - a mode, and a mode is where the wrong camera gets deleted. Putting
   * them on the row makes the target the thing you clicked.
   */
  let {
    cameras,
    busy,
    onaction,
  }: {
    cameras: (StoredCamera | undefined)[]
    busy: boolean
    onaction?: (action: 'add' | 'remove' | 'restore' | 'clear', index: number) => void
  } = $props()
</script>

<section class="cameras nodrag">
  <header>
    <span>Captured cameras</span><strong>{cameras.length}</strong>
  </header>

  {#if cameras.length > 0}
    <!--
      nowheel as well as nodrag: a long list scrolls inside the node, and a wheel
      that reached the canvas would zoom the graph while the user was reading the
      list they had just opened.
    -->
    <ol class="nowheel">
      {#each cameras as camera, index (index)}
        <li class:broken={camera === undefined}>
          <span class="index">{index + 1}</span>
          {#if camera === undefined}
            <!--
              ⚠️ DRAWN, NOT SKIPPED. The row still occupies its index, because the
              runtime still holds it - hiding it here would make every Remove
              below it delete a different camera than the one the user clicked.
            -->
            <code class="unreadable">unreadable</code>
          {:else}
            <code>{formatPosition(camera.eye)}</code>
            <em>{camera.viewConeDegreesHorizontal.toFixed(0)}&deg;</em>
            <!--
              The sun this camera was taken under, as a marker rather than a
              column: the row is already tight, and what a reader needs at a
              glance is WHETHER this frame carries its own lighting. The angles
              themselves are in the title, in compass terms.
            -->
            {#if camera.hasSun}
              <span class="sun" title={formatSun(camera)}>&#9728;</span>
            {:else}
              <span class="sun none" title="No sun was captured with this camera">&middot;</span>
            {/if}
          {/if}
          <button
            type="button"
            disabled={busy || onaction === undefined || camera === undefined}
            title="Point Archicad's 3D window at this camera"
            onclick={() => onaction?.('restore', index)}>Restore</button
          >
          <button
            type="button"
            class="remove"
            disabled={busy || onaction === undefined}
            title="Remove this camera from the list"
            onclick={() => onaction?.('remove', index)}>&times;</button
          >
        </li>
      {/each}
    </ol>
  {:else}
    <p>Point Archicad's 3D window where you want it, then press Add.</p>
  {/if}

  <div class="actions">
    <button type="button" disabled={busy || onaction === undefined} onclick={() => onaction?.('add', 0)}>Add</button>
    <button
      type="button"
      disabled={busy || onaction === undefined || cameras.length === 0}
      onclick={() => onaction?.('clear', 0)}>Clear</button
    >
  </div>
</section>

<style>
  .cameras { padding: 0 9px 9px 11px; }
  header { display: flex; align-items: baseline; justify-content: space-between; padding: 4px 0 2px; }
  header span { color: var(--text-muted); font-size: 9px; }
  header strong { color: var(--node-color); font: 600 14px/1 ui-monospace, monospace; }
  p { margin: 0; padding: 4px 0; color: var(--text-faint); font-size: 8px; }
  ol { max-height: 116px; margin: 0; padding: 0; overflow-y: auto; list-style: none; }
  li { display: grid; grid-template-columns: 12px 1fr auto auto auto auto; align-items: center; gap: 3px; padding: 1px 0; }
  .index { color: var(--text-faint); font: 9px/1 ui-monospace, monospace; text-align: right; }
  code { overflow: hidden; color: var(--text); font: 9px/1 ui-monospace, monospace; text-overflow: ellipsis; white-space: nowrap; }
  .unreadable { color: var(--text-faint); font-style: italic; }
  em { color: var(--text-faint); font: 8px/1 ui-monospace, monospace; font-style: normal; }
  .sun { width: 9px; color: var(--text-muted); font-size: 8px; text-align: center; cursor: help; }
  .sun.none { color: var(--text-faint); }
  button { height: 17px; padding: 0 4px; font-size: 7px; }
  .remove { width: 17px; padding: 0; }
  .actions { display: grid; grid-template-columns: repeat(2, 1fr); margin-top: 4px; gap: 2px; }
  .actions button { height: 22px; font-size: 7px; }
</style>
