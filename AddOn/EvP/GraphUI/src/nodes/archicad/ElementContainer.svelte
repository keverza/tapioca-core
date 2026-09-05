<script lang="ts">
  /**
   * ONE ELEMENT TYPE'S CONTAINER: a bar saying what it holds, which opens into
   * the classification-sensitive settings for the elements inside it.
   *
   * ⚠️ THE BAR DRAWS OFFLINE; ONLY OPENING IT ASKS ARCHICAD. The type and the
   * count come from what the selection node captured when the user pressed a
   * button, so a stack of six containers is six reads of the document and no
   * round trips - a node that queried the model to draw its own body would make
   * every repaint a gate crossing. The SETTINGS are live, fetched on the open,
   * because a stale thickness is worse than a slow one.
   *
   * ⚠️ AND IT NEVER RENDERS A LABEL IT WAS NOT GIVEN. The setting names, groups
   * and order arrive in the same response as the values; see elements.ts.
   */
  import type {
    ElementDescription,
    ElementDescriptionResponse,
    ElementGroup,
    ElementTypeSchema,
    SettingMenuTarget,
  } from '../../types'
  import { settingSectionsOf, unreadSettingCount, type SettingRow } from './elements'

  let {
    group,
    ondescribe,
    onsettingmenu,
  }: {
    group: ElementGroup
    ondescribe?: (guids: string[]) => Promise<ElementDescriptionResponse>
    /**
     * A right-click on one settings row. Raised rather than handled, because the
     * editor owns THE context menu - three menus in three sizes on screen at
     * once is what happens when each panel draws its own.
     */
    onsettingmenu?: (target: SettingMenuTarget) => void
  } = $props()

  let open = $state(false)
  let busy = $state(false)
  let failure = $state('')
  let described = $state.raw<ElementDescription[]>([])
  let schemas = $state.raw<ElementTypeSchema[]>([])
  let truncated = $state(false)
  let expandedGuid = $state('')

  // The elements a single open reads. A container holding four hundred walls is
  // a legitimate thing to have; a settings panel listing four hundred of them is
  // not, and reading them all to draw a list nobody scrolls costs the gate.
  const PAGE = 25

  const shown = $derived(group.guids.slice(0, PAGE))
  const hidden = $derived(Math.max(0, group.guids.length - PAGE))

  async function toggle(): Promise<void> {
    if (open) {
      open = false
      return
    }
    open = true
    if (described.length > 0 || ondescribe === undefined) return
    busy = true
    failure = ''
    try {
      const response = await ondescribe(shown)
      // `ok` false is a REPORTED outcome - no project open, most often - and it
      // belongs in the panel rather than as an exception nobody catches.
      if (!response.ok) failure = response.error
      described = response.elements
      schemas = response.types
      truncated = response.truncated
    } catch (error) {
      failure = error instanceof Error ? error.message : String(error)
    } finally {
      busy = false
    }
  }

  function schemaFor(element: ElementDescription): ElementTypeSchema | undefined {
    return schemas.find((schema) => schema.id === element.elementType)
  }

  /**
   * Right-click on a settings row.
   *
   * ⚠️ IT CARRIES THE GROUP'S TYPE, NOT THE ELEMENT'S. A promotion is made
   * against a TYPE's schema and applies to every element the host produces, so
   * the row that was clicked names the setting and the container names the type.
   * Sending the clicked element's own guid would suggest the promotion is about
   * that one element, which it is not.
   */
  function rowMenu(event: MouseEvent, row: SettingRow): void {
    if (onsettingmenu === undefined) return
    event.preventDefault()
    event.stopPropagation()
    onsettingmenu({
      settingId: row.id,
      label: row.label,
      elementType: group.elementType,
      x: event.clientX,
      y: event.clientY,
    })
  }

  function title(element: ElementDescription): string {
    const id = element.settings.find((setting) => setting.id === 'elementId')?.text ?? ''
    // The user's own element ID when it has one, and the guid when it does not -
    // never a bare index, which is a name for a row rather than for an element.
    return id !== '' ? id : element.guid
  }
</script>

<section class="container" class:open>
  <button type="button" class="bar" onclick={() => void toggle()} aria-expanded={open}>
    <span class="chevron" aria-hidden="true">{open ? '▾' : '▸'}</span>
    <span class="label">{group.label}</span>
    <span class="count">{group.guids.length}</span>
  </button>

  {#if open}
    <div class="contents nowheel">
      {#if busy}
        <p class="note">Reading from Archicad...</p>
      {:else if failure !== ''}
        <p class="note failure">{failure}</p>
      {:else if described.length === 0}
        <p class="note">Nothing came back for these elements.</p>
      {:else}
        {#each described as element (element.guid)}
          {@const schema = schemaFor(element)}
          {@const sections = settingSectionsOf(element, schema)}
          {@const unread = unreadSettingCount(element, schema)}
          <article>
            <button
              type="button"
              class="element"
              onclick={() => (expandedGuid = expandedGuid === element.guid ? '' : element.guid)}
              aria-expanded={expandedGuid === element.guid}
            >
              <span class="chevron" aria-hidden="true">{expandedGuid === element.guid ? '▾' : '▸'}</span>
              <span class="name">{title(element)}</span>
              <span class="type">{element.typeLabel}</span>
            </button>

            {#if expandedGuid === element.guid}
              {#if !element.available}
                <!-- A captured element the project no longer has. Said plainly:
                     an empty settings list would read as "this wall has no
                     settings" rather than "this wall is gone". -->
                <p class="note failure">{element.detail}</p>
              {:else}
                {#each sections as section (section.group)}
                  <h4>{section.group}</h4>
                  <dl>
                    {#each section.rows as row (row.id)}
                      <!--
                        ⚠️ A DERIVED ROW SAYS SO. Archicad has no wall length -
                        it has two endpoints and an angle - so this number is
                        arithmetic Tapioca did. A user checking the panel against
                        a schedule needs to know that a disagreement here is
                        about the DEFINITION of length rather than about the
                        reader, and the marker is the only thing that tells them.
                      -->
                      <!--
                        ⚠️ THE WHOLE ROW IS THE TARGET, dt AND dd BOTH. A menu
                        that only opened over the label would make the value -
                        the half a user is looking at when they decide they want
                        it downstream - the one place right-click does nothing.
                      -->
                      <dt oncontextmenu={(event) => rowMenu(event, row)}>
                        {row.label}{#if row.origin === 'derived'}<abbr title="Computed by Tapioca; Archicad has no such field">ƒ</abbr>{/if}
                      </dt>
                      <dd oncontextmenu={(event) => rowMenu(event, row)}>{row.text}{#if row.unit !== ''}<em> {row.unit}</em>{/if}</dd>
                    {/each}
                  </dl>
                {/each}
                {#if sections.length === 0}
                  <p class="note">This build reads none of this type's own settings yet.</p>
                {:else if unread > 0}
                  <!-- ⚠️ SAID, NOT HIDDEN. The table declares more settings for
                       this type than the reader fills; a user comparing the panel
                       against Archicad's own dialog deserves to know the panel is
                       short rather than concluding the field is empty. -->
                  <p class="note">{unread} further setting{unread === 1 ? '' : 's'} this build does not read yet.</p>
                {/if}
              {/if}
            {/if}
          </article>
        {/each}

        {#if hidden > 0}
          <p class="note">{hidden} more not read - open fewer elements at a time.</p>
        {/if}
        {#if truncated}
          <p class="note">The runtime capped this read.</p>
        {/if}
      {/if}
    </div>
  {/if}
</section>

<style>
  .container { border: 1px solid var(--border); border-radius: 2px; background: var(--canvas); }
  .container + :global(.container) { margin-top: 3px; }
  .bar {
    display: flex;
    width: 100%;
    height: 22px;
    align-items: center;
    padding: 0 6px;
    border: 0;
    border-radius: 0;
    background: var(--surface-raised);
    color: var(--text);
    cursor: pointer;
    gap: 5px;
  }
  .bar:hover { filter: brightness(1.12); }
  .open .bar { border-bottom: 1px solid var(--border); }
  .chevron { width: 8px; color: var(--text-faint); font-size: 8px; }
  .label { overflow: hidden; flex: 1 1 auto; font: 550 9px/1 'Segoe UI', sans-serif; text-align: left; text-overflow: ellipsis; white-space: nowrap; }
  .count { padding: 0 4px; border-radius: 7px; background: var(--canvas); color: var(--text-faint); font: 8px/13px ui-monospace, monospace; }
  .contents { max-height: 190px; padding: 3px; overflow: auto; }
  .element { display: flex; width: 100%; height: 19px; align-items: center; padding: 0 4px; border: 0; border-radius: 0; background: transparent; color: var(--text); cursor: pointer; gap: 5px; }
  .element:hover { background: var(--surface-raised); }
  .name { overflow: hidden; flex: 1 1 auto; font: 9px/1 ui-monospace, monospace; text-align: left; text-overflow: ellipsis; white-space: nowrap; }
  .type { color: var(--text-faint); font: 8px/1 'Segoe UI', sans-serif; }
  h4 { margin: 5px 0 2px; padding-left: 17px; color: var(--text-faint); font: 8px/1 ui-monospace, monospace; letter-spacing: .09em; text-transform: uppercase; }
  dl { display: grid; margin: 0; padding: 0 4px 0 17px; grid-template-columns: 1fr auto; gap: 1px 8px; }
  dt { color: var(--text-faint); font: 9px/1.5 'Segoe UI', sans-serif; }
  dt abbr { margin-left: 3px; border: 0; color: var(--text-faint); cursor: help; font: italic 8px/1 'Segoe UI', serif; opacity: .75; text-decoration: none; }
  dd { overflow: hidden; margin: 0; font: 9px/1.5 ui-monospace, monospace; text-align: right; text-overflow: ellipsis; white-space: nowrap; }
  dd em { color: var(--text-faint); font-style: normal; }
  .note { margin: 4px 6px; color: var(--text-faint); font: 9px/1.4 'Segoe UI', sans-serif; }
  .failure { color: var(--danger); }
</style>
