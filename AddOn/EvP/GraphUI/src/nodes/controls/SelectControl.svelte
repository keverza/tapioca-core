<script lang="ts">
  /**
   * One value chosen from a list the RUNTIME supplies.
   *
   * Two sources, one control: literal options declared by the node type, and the
   * Archicad attribute domains, which cannot be static catalog data because they
   * belong to the open project. Neither is enumerated here - this component asks
   * for a domain and renders what came back.
   *
   * ⚠️ AN UNKNOWN VALUE IS SHOWN, NOT CORRECTED. A graph saved against another
   * project can carry a layer name this one does not have. Snapping it to the
   * first option would rewrite the user's document without being asked, and the
   * rewrite would be invisible; so the missing choice stays selected and says so.
   */
  import type { GraphValue, ParameterOption, ParameterOptionSource } from '../../types'
  import { isKnownOption, optionKey, textOf } from './widgets'

  let {
    value,
    options,
    source,
    label,
    disabled = false,
    loading = false,
    oncommit,
    onrequest,
  }: {
    value: GraphValue | undefined
    options: ParameterOption[]
    source: ParameterOptionSource
    label: string
    disabled?: boolean
    loading?: boolean
    oncommit: (text: string) => void
    onrequest?: (source: ParameterOptionSource) => void
  } = $props()

  const current = $derived(optionKey(value))
  const known = $derived(isKnownOption(options, value))
  const missing = $derived(!known ? textOf(value) : '')

  /**
   * Ask for the domain once the row is on screen.
   *
   * ⚠️ THE CONDITION IS "NOT ANSWERED YET", NOT "NOT LOADING". Guarding this on
   * `!loading` was a deadlock: `loading` IS the never-asked state, so the request
   * that would clear it could never fire and every picker sat on "Listing…"
   * forever. Repeat calls are harmless - the editor keeps one in-flight request
   * per domain and the listing is a project-wide read shared by every picker.
   */
  $effect(() => {
    if (source !== 'none' && options.length === 0) onrequest?.(source)
  })

  function commit(event: Event): void {
    const chosen = (event.currentTarget as HTMLSelectElement).value
    if (chosen === current) return
    oncommit(chosen)
  }
</script>

<select class="select nodrag" value={current} disabled={disabled || (options.length === 0 && missing === '')} aria-label={label} onchange={commit}>
  {#if missing !== ''}
    <!-- Kept selectable so re-choosing it is possible after a mis-click, and
         named so the user can see WHICH attribute the project is missing. -->
    <option value={current}>{missing} (not in this project)</option>
  {/if}
  {#if options.length === 0 && missing === ''}
    <option value="">{loading ? 'Listing…' : source === 'none' ? 'No options' : 'None in this project'}</option>
  {/if}
  {#each options as option (optionKey(option.value))}
    <option value={optionKey(option.value)}>{option.label}</option>
  {/each}
</select>

<style>
  .select { width: 100%; max-width: none; height: 19px; padding: 0 4px 0 5px; border: 1px solid transparent; border-radius: 2px; background: var(--canvas); color: var(--text); font: 9px/1.2 'Segoe UI', sans-serif; }
  .select:hover:not(:disabled) { border-color: var(--border); background: var(--canvas); }
  .select:focus { border-color: var(--node-color); outline: none; }
  .select:disabled { color: var(--text-faint); opacity: 1; }
</style>
