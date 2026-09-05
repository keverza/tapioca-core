<script lang="ts">
  import { assemblyIsStale, commitBlockerText, type CommitAssembly, type CommitLamp } from './commit'

  let {
    lamp,
    assembly,
    revision,
    nodeCount,
    durationMs,
    busy,
    committing,
    nativeConnected,
    open = $bindable(false),
    onrun,
    onaccept,
  }: {
    lamp: CommitLamp
    assembly: CommitAssembly
    /** The graph's revision NOW, which is what makes an assembly stale. */
    revision: number
    nodeCount: number
    durationMs: number
    busy: boolean
    committing: boolean
    nativeConnected: boolean
    open?: boolean
    onrun: () => void
    onaccept: () => void
  } = $props()

  const LAMP_TEXT: Record<CommitLamp, string> = {
    live: 'Live',
    running: 'Running',
    locked: 'Locked',
    failed: 'Failed',
    committed: 'Committed',
  }

  const stale = $derived(assemblyIsStale(assembly, revision))

  /*
    ⚠️ A STALE ASSEMBLY CANNOT BE COMMITTED. The list on screen was built at a
    revision the graph has since left, so the nodes it names are not necessarily
    the nodes that would be written. Rebuilding is one press of RUN; committing
    a list the user is no longer looking at is not recoverable.
  */
  const canAccept = $derived(
    nativeConnected && !busy && !committing && !stale && assembly.ready.length > 0,
  )

  const acceptTitle = $derived(
    !nativeConnected
      ? 'Committing needs the native graph runtime.'
      : stale
        ? `Built at revision ${assembly.revision}; the graph is at ${revision}. Run again to rebuild the list.`
        : assembly.ready.length > 0
          ? `Commit ${assembly.ready.length} node${assembly.ready.length === 1 ? '' : 's'} to Archicad`
          : commitBlockerText(assembly.blocker),
  )
</script>

<div class="commit-bar" data-lamp={lamp}>
  <span class="lamp" aria-hidden="true"></span>
  <span class="lamp-text">{LAMP_TEXT[lamp]}</span>
  <span class="stats">{nodeCount} node{nodeCount === 1 ? '' : 's'}{durationMs > 0 ? ` · ${Math.round(durationMs)} ms` : ''}</span>

  <button
    type="button"
    class="assembly-toggle"
    aria-expanded={open}
    disabled={assembly.rows.length === 0}
    onclick={() => (open = !open)}
    title={assembly.rows.length === 0 ? commitBlockerText(assembly.blocker) : 'What ACCEPT would write'}
  >
    {assembly.rows.length === 0 ? 'nothing to commit' : `${assembly.rows.length} to commit`}
    <span class="caret" aria-hidden="true">{open ? '▴' : '▾'}</span>
  </button>

  <!--
    RUN IS SAFE BY CONSTRUCTION and can be pressed at any time: it forces a
    re-evaluation and writes nothing, and it is also the resume when the
    solution is locked. That is why only ACCEPT is guarded.
  -->
  <button type="button" class="run" disabled={!nativeConnected || busy} onclick={onrun}>
    {lamp === 'locked' ? 'RESUME' : 'RUN'}
  </button>
  <button type="button" class="accept" disabled={!canAccept} onclick={onaccept} title={acceptTitle}>
    {committing ? 'COMMITTING…' : 'ACCEPT'}
  </button>
</div>

{#if open && assembly.rows.length > 0}
  <div class="assembly" role="region" aria-label="What ACCEPT would write">
    <div class="assembly-head">
      <span>{assembly.rows.length} to commit</span>
      <code class:stale>rev {assembly.revision}{stale ? ` · stale, graph is at ${revision}` : ''}</code>
    </div>
    <!--
      Blocked rows are LISTED, not dropped. A node that cannot commit because of
      an upstream error is the most important row here, and hiding it is how a
      user comes to believe they committed something they did not.
    -->
    {#each assembly.rows as row (row.nodeId)}
      <div class="assembly-row" data-state={row.state}>
        <span class="row-name">{row.name}</span>
        <span class="row-writes">{row.writes}{row.count >= 0 ? ` · ${row.count} item${row.count === 1 ? '' : 's'}` : ''}</span>
        <span class="row-state">{row.state === 'ready' ? 'ready' : row.detail === '' ? row.state : `${row.state} — ${row.detail}`}</span>
      </div>
    {/each}
  </div>
{/if}
