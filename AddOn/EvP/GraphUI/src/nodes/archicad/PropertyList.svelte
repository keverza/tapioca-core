<script lang="ts">
  /**
   * WHAT A WALL HAS, and the button that puts one of them on the canvas.
   *
   * ⚠️ THIS IS THE PANEL THE BRIEF ASKED FOR. The value tree beside it answers
   * "what did this node produce" - a list of GUIDs, which is the identity and
   * not a property. This answers "what can I take from it", which is the
   * question somebody opens the browser to ask, and it is the only place a
   * promotion can start.
   *
   * ⚠️ IT SHOWS THE SCHEMA, NOT THE ELEMENTS (§12). One row per property of the
   * type, with the elements' shared value beside it or a count when they differ.
   * A row per element per property would be sixteen hundred rows for four
   * hundred walls, and the ones that agree carry no information.
   */
  import type { ElementDescription, ElementDescriptionResponse, ElementGroup, SettingMenuTarget } from '../../types'
  import { elementDisplayName, propertyListOf, type PropertyList, type PropertyRow } from './elements'

  let {
    groups = [],
    ondescribe,
    onpromote,
    onmenu,
    promoted = [],
  }: {
    groups?: ElementGroup[]
    ondescribe?: (guids: string[]) => Promise<ElementDescriptionResponse>
    onpromote?: (target: SettingMenuTarget) => void
    onmenu?: (target: SettingMenuTarget) => void
    /** Setting ids already promoted on this node, so the row can say so. */
    promoted?: string[]
  } = $props()

  let busy = $state(false)
  let failure = $state('')
  let described = $state.raw<ElementDescription[]>([])
  let lists = $state.raw<PropertyList[]>([])
  let openGroups = $state<Record<string, boolean>>({})
  let query = $state('')
  let loadedFor = $state('')

  /**
   * How many elements one read looks at.
   *
   * ⚠️ A SAMPLE, AND IT IS HONEST ABOUT BEING ONE. The panel needs to know what
   * a wall HAS, which the first few answer as well as four hundred would; the
   * previews are the sample's, and the footer says so. Reading four hundred to
   * fill a column nobody scrolls costs the main-thread gate four hundred times.
   */
  const SAMPLE = 12

  const key = $derived(groups.map((group) => `${group.elementType}:${group.guids.length}`).join('|'))

  $effect(() => {
    if (key === loadedFor || groups.length === 0 || ondescribe === undefined) return
    loadedFor = key
    void load()
  })

  async function load(): Promise<void> {
    if (ondescribe === undefined) return
    busy = true
    failure = ''
    try {
      const sampled = groups.flatMap((group) => group.guids.slice(0, SAMPLE))
      const response = await ondescribe(sampled)
      if (!response.ok) failure = response.error
      described = response.elements
      lists = response.types
        .map((schema) =>
          propertyListOf(
            schema,
            response.elements.filter((element) => element.elementType === schema.id),
          ),
        )
        .filter((list): list is PropertyList => list !== undefined)
      // Identity open, the rest closed: the shape is what you want first.
      openGroups = Object.fromEntries(lists.flatMap((list) => list.groups.map((g) => [`${list.elementType}/${g.group}`, g.group === 'Identity'])))
    } catch (error) {
      failure = error instanceof Error ? error.message : String(error)
    } finally {
      busy = false
    }
  }

  function matches(row: PropertyRow): boolean {
    const text = query.trim().toLowerCase()
    if (text === '') return true
    return (
      row.label.toLowerCase().includes(text) ||
      row.settingId.toLowerCase().includes(text) ||
      row.valueType.toLowerCase().includes(text) ||
      row.preview.toLowerCase().includes(text)
    )
  }

  function target(list: PropertyList, row: PropertyRow): SettingMenuTarget {
    return { settingId: row.settingId, label: row.label, elementType: list.elementType, x: 0, y: 0 }
  }

  function menu(event: MouseEvent, list: PropertyList, row: PropertyRow): void {
    if (onmenu === undefined) return
    event.preventDefault()
    event.stopPropagation()
    onmenu({ ...target(list, row), x: event.clientX, y: event.clientY })
  }

  function sampled(list: PropertyList): boolean {
    const group = groups.find((candidate) => candidate.elementType === list.elementType)
    return (group?.guids.length ?? 0) > SAMPLE
  }
</script>

<section class="properties">
  <label>
    <span>Properties</span>
    <input bind:value={query} placeholder="Name, type or value" />
  </label>

  {#if busy}
    <p class="note">Reading from Archicad...</p>
  {:else if failure !== ''}
    <p class="note failure">{failure}</p>
  {:else if groups.length === 0}
    <p class="note">This node holds no Archicad elements. Run it, or wire it to a selection.</p>
  {:else if lists.length === 0}
    <p class="note">Nothing came back for these elements.</p>
  {:else}
    {#each lists as list (list.elementType)}
      <h4>
        <span>{list.label}</span>
        <span class="count">{list.elementCount}</span>
      </h4>

      <!--
        WHICH elements, by a name a person recognises. The guid is the identity
        and stays in the value tree beside this; it is not a name.
      -->
      <p class="members">
        {described
          .filter((element) => element.elementType === list.elementType)
          .map(elementDisplayName)
          .join(', ')}
      </p>

      {#each list.groups as group (group.group)}
        {@const rows = group.rows.filter(matches)}
        {#if rows.length > 0}
          {@const groupKey = `${list.elementType}/${group.group}`}
          {@const open = openGroups[groupKey] ?? query.trim() !== ''}
          <button type="button" class="group" onclick={() => (openGroups = { ...openGroups, [groupKey]: !open })} aria-expanded={open}>
            <span class="chevron" aria-hidden="true">{open ? '▾' : '▸'}</span>
            {group.group}
          </button>
          {#if open}
            {#each rows as row (row.settingId)}
              {@const already = promoted.includes(row.settingId)}
              <div
                class="row"
                class:unread={row.unread}
                oncontextmenu={(event) => menu(event, list, row)}
                role="presentation"
              >
                <span class="name">
                  {row.label}{#if row.origin === 'derived'}<abbr title="Computed by Tapioca; Archicad has no such field">ƒ</abbr>{/if}
                </span>
                <!--
                  ⚠️ THE VALUE IS SUBDUED AND THE TYPE IS NOT (§19). The graph has
                  to stay readable with no Archicad selection open, and the type
                  is the half that is still true then.
                -->
                <span class="value">{row.showUnit ? `${row.preview} ${row.unit}` : row.preview}</span>
                <span class="type">{row.valueType}</span>
                <button
                  type="button"
                  class="promote"
                  disabled={already || onpromote === undefined}
                  title={already ? 'Already promoted on this node' : `Promote ${row.label}`}
                  aria-label={already ? `${row.label} is already promoted` : `Promote ${row.label}`}
                  onclick={() => onpromote?.(target(list, row))}>{already ? '✓' : '＋'}</button>
              </div>
            {/each}
          {/if}
        {/if}
      {/each}

      {#if sampled(list)}
        <!-- ⚠️ SAID, NOT HIDDEN. The values beside these rows are a sample's. -->
        <p class="note">Values sampled from the first {SAMPLE} of {list.elementCount}.</p>
      {/if}
    {/each}
  {/if}
</section>

<style>
  .properties { display: grid; align-content: start; gap: 2px; }
  label { display: grid; padding: 0 0 6px; gap: 5px; }
  label span { color: var(--text-faint); font: 8px/1 ui-monospace, monospace; letter-spacing: .1em; text-transform: uppercase; }
  input { width: 100%; height: 27px; padding: 0 7px; }
  h4 { display: flex; align-items: center; margin: 8px 0 1px; gap: 6px; font: 550 9px/1 'Segoe UI', sans-serif; }
  .count { padding: 0 4px; border-radius: 7px; background: var(--canvas); color: var(--text-faint); font: 8px/13px ui-monospace, monospace; }
  .members { overflow: hidden; margin: 0 0 4px; color: var(--text-faint); font: 8px/1.4 ui-monospace, monospace; text-overflow: ellipsis; white-space: nowrap; }
  .group { display: flex; width: 100%; height: 18px; align-items: center; padding: 0 2px; border: 0; border-radius: 0; background: transparent; color: var(--text-faint); cursor: pointer; font: 8px/1 ui-monospace, monospace; gap: 4px; letter-spacing: .09em; text-align: left; text-transform: uppercase; }
  .group:hover { color: var(--text); }
  .chevron { width: 8px; font-size: 8px; }
  .row { display: grid; align-items: center; padding: 0 2px 0 15px; gap: 6px; grid-template-columns: minmax(0, 1fr) auto auto 20px; }
  .row:hover { background: var(--surface-raised); }
  .row.unread { opacity: .55; }
  .name { overflow: hidden; font: 9px/18px 'Segoe UI', sans-serif; text-overflow: ellipsis; white-space: nowrap; }
  .name abbr { margin-left: 3px; border: 0; color: var(--text-faint); cursor: help; font: italic 8px/1 'Segoe UI', serif; text-decoration: none; }
  .value { overflow: hidden; max-width: 92px; color: var(--text-faint); font: 9px/18px ui-monospace, monospace; text-align: right; text-overflow: ellipsis; white-space: nowrap; }
  .type { color: var(--text-faint); font: 8px/18px ui-monospace, monospace; opacity: .75; }
  .promote { width: 18px; height: 18px; padding: 0; border: 1px solid var(--border); border-radius: 2px; background: var(--canvas); color: var(--text); font-size: 9px; line-height: 1; }
  .promote:hover:not(:disabled) { border-color: var(--node-color); }
  .promote:disabled { border-color: transparent; background: transparent; color: var(--text-faint); }
  .note { margin: 6px 2px; color: var(--text-faint); font: 9px/1.4 'Segoe UI', sans-serif; }
  .failure { color: var(--danger); }
</style>
