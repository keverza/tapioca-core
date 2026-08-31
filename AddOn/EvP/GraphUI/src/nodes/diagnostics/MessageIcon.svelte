<script lang="ts">
  import type { NodeResultRecord } from '../../types'

  let { status, message, onclick }: { status: NodeResultRecord['status'] | 'pending'; message?: string; onclick: () => void } = $props()
  const noteworthy = $derived(status === 'error' || status === 'blocked')
</script>

<button class:bad={status === 'error'} class:noteworthy type="button" title={message || status} {onclick} aria-label={`Node status: ${status}`}>{status}</button>

<style>
  button { overflow: hidden; max-width: 52px; height: 22px; padding: 0 3px; border-color: transparent; background: transparent; color: var(--text-faint); font: 7px/1 ui-monospace, monospace; text-overflow: ellipsis; text-transform: uppercase; white-space: nowrap; }
  button.noteworthy { color: #c9922f; }
  button.bad { color: var(--danger); }
</style>
