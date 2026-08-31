<script lang="ts">
  import type { NodeResultRecord } from '../../types'
  let { result, onclose }: { result?: NodeResultRecord; onclose: () => void } = $props()
</script>

<aside class="nodrag" aria-label="Node message">
  <header><strong>{result?.status ?? 'Pending'}</strong><button type="button" onclick={onclose}>Close</button></header>
  <p>{result?.message || 'No runtime message.'}</p>
  {#if result?.code}<code>{result.code}</code>{/if}
</aside>

<style>
  aside { position: absolute; z-index: 13; top: 31px; right: 5px; width: 220px; padding: 8px; border: 1px solid var(--border); background: var(--surface); box-shadow: 0 12px 28px rgb(0 0 0 / 45%); }
  header { display: flex; align-items: center; justify-content: space-between; }
  header strong { font-size: 10px; text-transform: capitalize; }
  button { height: 23px; padding: 0 6px; font-size: 8px; }
  p { margin: 8px 0; color: var(--text-muted); font-size: 9px; line-height: 1.4; }
  code { color: var(--text-faint); font-size: 8px; }
</style>
