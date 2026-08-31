<script lang="ts">
  import type { NodeResultRecord } from '../types'
  import MessageIcon from './diagnostics/MessageIcon.svelte'
  import MessagePopover from './diagnostics/MessagePopover.svelte'

  let { result }: { result?: NodeResultRecord } = $props()
  let open = $state(false)
</script>

<MessageIcon status={result?.status ?? 'pending'} message={result?.message} onclick={() => (open = !open)} />
{#if open}<MessagePopover {result} onclose={() => (open = false)} />{/if}
