<script lang="ts">
  import type { NodeResultRecord } from '../../types'
  import { formatDiagnostics, type ComponentMessage } from '../types/diagnostics'
  let { result, messages, onclose }: { result?: NodeResultRecord; messages: ComponentMessage[]; onclose: () => void } = $props()
  const fallback = $derived.by((): ComponentMessage[] => result?.message ? [{ severity: result.status === 'error' ? 'error' : 'info', code: result.code ?? result.status, title: result.message, nodeId: result.nodeId }] : [])
  const allMessages = $derived(messages.length > 0 ? messages : fallback)
  function copy(text: string): void { void navigator.clipboard?.writeText(text) }
</script>

<aside class="nodrag nowheel" aria-label="Node message">
  <header><strong>{result?.status ?? 'Pending'}</strong><button type="button" onclick={onclose}>Close</button></header>
  {#each allMessages as message}<p><strong>{message.title}</strong>{#if message.detail}<span>{message.detail}</span>{/if}<code>{message.code}{message.portId ? ` / ${message.portId}` : ''}</code></p>{:else}<p>No runtime message.</p>{/each}
  <footer><button type="button" disabled={allMessages.length === 0} onclick={() => copy(allMessages[0]?.title ?? '')}>Copy message</button><button type="button" disabled={allMessages.length === 0} onclick={() => copy(formatDiagnostics(allMessages))}>Copy diagnostics</button></footer>
</aside>

<style>
  /*
   * ABOVE the node, not over it. `article` is the positioned ancestor, so
   * anchoring to its top edge puts the box clear of the body it is describing -
   * a status box that covers the node's own rows hides the thing the message is
   * about.
   */
  aside { position: absolute; z-index: 19; bottom: calc(100% + 9px); right: -1px; width: 230px; padding: 8px; border: 1px solid var(--border); border-top: 2px solid var(--node-color); border-radius: 3px; background: var(--surface); box-shadow: 0 14px 34px rgb(0 0 0 / 46%); text-align: left; }
  header { display: flex; align-items: center; justify-content: space-between; }
  header strong { font-size: 10px; text-transform: capitalize; }
  button { height: 23px; padding: 0 6px; font-size: 8px; }
  p { display: grid; margin: 8px 0; color: var(--text-muted); font-size: 9px; line-height: 1.4; gap: 4px; }
  p strong { color: var(--text); }
  p span { color: var(--text-muted); }
  code { color: var(--text-faint); font-size: 8px; }
  footer { display: flex; gap: 4px; border-top: 1px solid var(--border); padding-top: 7px; }
</style>
