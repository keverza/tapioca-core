<script lang="ts">
  import type { NodeResultRecord } from '../types'
  import MessageIcon from './diagnostics/MessageIcon.svelte'
  import MessagePopover from './diagnostics/MessagePopover.svelte'
  import { aggregateSeverity, type ComponentMessage } from './types/diagnostics'

  let { result, messages = [] }: { result?: NodeResultRecord; messages?: ComponentMessage[] } = $props()
  let open = $state(false)
  const severity = $derived(aggregateSeverity(messages))
</script>

<MessageIcon status={severity === 'error' ? 'error' : severity === 'warning' ? 'blocked' : result?.status ?? 'pending'} message={messages[0]?.title ?? result?.message} onclick={() => (open = !open)} />
{#if open}<MessagePopover {result} {messages} onclose={() => (open = false)} />{/if}
