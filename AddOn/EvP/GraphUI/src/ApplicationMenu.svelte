<script lang="ts">
  import { onMount } from 'svelte'
  import type { ThemeMode } from './editor'

  type MenuId = 'file' | 'run' | 'flow' | 'debug'
  type FileAction = 'new' | 'load' | 'save'

  let {
    busy,
    nativeConnected,
    snapEnabled,
    theme,
    performanceOpen,
    onrefresh,
    onevaluate,
    onevaluatesequential,
    solutionLocked,
    ontogglelock,
    onfileaction,
    onfit,
    ontogglesnap,
    ontheme,
    ontoggleperformance,
  }: {
    busy: boolean
    nativeConnected: boolean
    snapEnabled: boolean
    theme: ThemeMode
    performanceOpen: boolean
    onrefresh: () => void
    onevaluate: () => void
    onevaluatesequential: () => void
    solutionLocked: boolean
    ontogglelock: () => void
    onfileaction: (action: FileAction) => void
    onfit: () => void
    ontogglesnap: () => void
    ontheme: (theme: ThemeMode) => void
    ontoggleperformance: () => void
  } = $props()

  const menuIds: MenuId[] = ['file', 'run', 'flow', 'debug']
  let activeMenu = $state<MenuId | null>(null)
  let displayOptionsOpen = $state(false)
  let root = $state<HTMLElement>()
  let displayOptionsButton = $state<HTMLButtonElement>()
  let headingElements = $state.raw<Partial<Record<MenuId, HTMLButtonElement>>>({})
  let hoverMenus = false

  onMount(() => {
    hoverMenus = window.matchMedia('(hover: hover) and (pointer: fine)').matches
    const closeOutside = (event: PointerEvent) => {
      if (root !== undefined && !root.contains(event.target as globalThis.Node)) close()
    }
    window.addEventListener('pointerdown', closeOutside)
    return () => window.removeEventListener('pointerdown', closeOutside)
  })

  function setHeading(id: MenuId, element: HTMLButtonElement): void {
    headingElements = { ...headingElements, [id]: element }
  }

  function registerHeading(element: HTMLButtonElement, id: MenuId): { destroy: () => void } {
    setHeading(id, element)
    return {
      destroy: () => {
        const next = { ...headingElements }
        delete next[id]
        headingElements = next
      },
    }
  }

  function close(restoreFocus = true): void {
    const previous = activeMenu
    activeMenu = null
    displayOptionsOpen = false
    if (restoreFocus && previous !== null) queueMicrotask(() => headingElements[previous]?.focus())
  }

  function open(id: MenuId, focusFirst = false): void {
    if (activeMenu !== id) displayOptionsOpen = false
    activeMenu = id
    if (focusFirst) {
      queueMicrotask(() => {
        root?.querySelector<HTMLElement>(`#menu-${id} [role^="menuitem"]:not([disabled])`)?.focus()
      })
    }
  }

  function toggle(id: MenuId): void {
    if (activeMenu === id) close()
    else open(id)
  }

  function moveHeading(id: MenuId, offset: number): void {
    const index = menuIds.indexOf(id)
    const next = menuIds[(index + offset + menuIds.length) % menuIds.length]
    headingElements[next]?.focus()
    if (activeMenu !== null) open(next)
  }

  function headingKey(event: KeyboardEvent, id: MenuId): void {
    if (event.key === 'ArrowRight') moveHeading(id, 1)
    else if (event.key === 'ArrowLeft') moveHeading(id, -1)
    else if (event.key === 'ArrowDown') open(id, true)
    else if (event.key === 'Escape') close()
    else return
    event.preventDefault()
  }

  function menuKey(event: KeyboardEvent, id: MenuId): void {
    const items = Array.from(
      (event.currentTarget as HTMLElement).querySelectorAll<HTMLElement>('[role^="menuitem"]:not([disabled])'),
    )
    const index = items.indexOf(document.activeElement as HTMLElement)
    if (event.key === 'ArrowDown') items[(index + 1) % items.length]?.focus()
    else if (event.key === 'ArrowUp') items[(index - 1 + items.length) % items.length]?.focus()
    else if (event.key === 'Home') items[0]?.focus()
    else if (event.key === 'End') items.at(-1)?.focus()
    else if (event.key === 'ArrowRight') moveHeading(id, 1)
    else if (event.key === 'ArrowLeft') moveHeading(id, -1)
    else if (event.key === 'Escape') close()
    else return
    event.preventDefault()
  }

  function openDisplayOptions(focusFirst = false): void {
    displayOptionsOpen = true
    if (focusFirst) {
      queueMicrotask(() => {
        root?.querySelector<HTMLElement>('#menu-display-options [role^="menuitem"]')?.focus()
      })
    }
  }

  function displayOptionsButtonKey(event: KeyboardEvent): void {
    if (event.key !== 'ArrowRight' && event.key !== 'Enter' && event.key !== ' ') return
    openDisplayOptions(true)
    event.stopPropagation()
    event.preventDefault()
  }

  function displayOptionsKey(event: KeyboardEvent): void {
    const items = Array.from(
      (event.currentTarget as HTMLElement).querySelectorAll<HTMLElement>('[role^="menuitem"]'),
    )
    const index = items.indexOf(document.activeElement as HTMLElement)
    if (event.key === 'ArrowDown') items[(index + 1) % items.length]?.focus()
    else if (event.key === 'ArrowUp') items[(index - 1 + items.length) % items.length]?.focus()
    else if (event.key === 'Home') items[0]?.focus()
    else if (event.key === 'End') items.at(-1)?.focus()
    else if (event.key === 'ArrowLeft' || event.key === 'Escape') {
      displayOptionsOpen = false
      queueMicrotask(() => displayOptionsButton?.focus())
    } else return
    event.stopPropagation()
    event.preventDefault()
  }

  function invoke(action: () => void): void {
    close()
    action()
  }
</script>

<nav class="menu-bar" aria-label="Application menu" bind:this={root}>
  {#each menuIds as id}
    <div class="menu-root">
      <button
        use:registerHeading={id}
        class:open={activeMenu === id}
        type="button"
        aria-haspopup="menu"
        aria-expanded={activeMenu === id}
        onclick={() => toggle(id)}
        onpointerenter={() => {
          if (hoverMenus && activeMenu !== null && activeMenu !== id) open(id)
        }}
        onkeydown={(event) => headingKey(event, id)}
      >{id[0].toUpperCase() + id.slice(1)}</button>

      {#if activeMenu === id}
        <div id={`menu-${id}`} class:menu-right={id === 'debug'} class="menu-popover" role="menu" tabindex="-1" onkeydown={(event) => menuKey(event, id)}>
          {#if id === 'file'}
            <button role="menuitem" type="button" disabled={busy} onclick={() => invoke(() => onfileaction('new'))}>New graph</button>
            <button role="menuitem" type="button" disabled={busy} onclick={() => invoke(() => onfileaction('load'))}>Load graph...</button>
            <button role="menuitem" type="button" disabled={busy} onclick={() => invoke(() => onfileaction('save'))}>Save graph...</button>
            <div class="menu-separator" role="separator"></div>
            <button role="menuitem" type="button" disabled={busy || !nativeConnected} onclick={() => invoke(onrefresh)}>Refresh graph</button>
            <div
              class="menu-submenu-root"
              role="presentation"
              onpointerenter={() => openDisplayOptions()}
              onpointerleave={() => (displayOptionsOpen = false)}
            >
              <button
                bind:this={displayOptionsButton}
                class="submenu-trigger"
                role="menuitem"
                type="button"
                aria-haspopup="menu"
                aria-expanded={displayOptionsOpen}
                onclick={() => (displayOptionsOpen ? (displayOptionsOpen = false) : openDisplayOptions())}
                onkeydown={displayOptionsButtonKey}
              >Display Options<span aria-hidden="true">&#x203a;</span></button>
              {#if displayOptionsOpen}
                <div
                  id="menu-display-options"
                  class="menu-popover menu-submenu"
                  role="menu"
                  tabindex="-1"
                  onkeydown={displayOptionsKey}
                >
                  <button role="menuitemcheckbox" class="menu-toggle" type="button" aria-checked={snapEnabled} onclick={() => invoke(ontogglesnap)}>
                    <span class:checked={snapEnabled} aria-hidden="true"></span>
                    Snap to grid
                    <kbd>16 px</kbd>
                  </button>
                  <div class="menu-separator" role="separator"></div>
                  {#each ['system', 'dark', 'light'] as mode}
                    <button role="menuitemradio" class="menu-toggle" type="button" aria-checked={theme === mode} onclick={() => invoke(() => ontheme(mode as ThemeMode))}>
                      <span class:checked={theme === mode} aria-hidden="true"></span>
                      {mode[0].toUpperCase() + mode.slice(1)} theme
                    </button>
                  {/each}
                </div>
              {/if}
            </div>
          {:else if id === 'run'}
            <button role="menuitem" type="button" disabled={busy} onclick={() => invoke(onevaluate)}>Evaluate graph</button>
            <div class="menu-separator" role="separator"></div>
            <!-- The sequential arm of ADR-007's parallelism measurement. Same
                 graph, one thread, so the two status lines can be compared. -->
            <button role="menuitem" type="button" disabled={busy} onclick={() => invoke(onevaluatesequential)}>Evaluate one node at a time</button>
            <div class="menu-separator" role="separator"></div>
            <!-- Locking refuses evaluation and leaves editing open, so a batch
                 of edits costs one run rather than one run each. -->
            <button role="menuitemcheckbox" class="menu-toggle" type="button" aria-checked={solutionLocked} onclick={() => invoke(ontogglelock)}>
              <span class:checked={solutionLocked} aria-hidden="true"></span>
              Lock solution
            </button>
          {:else if id === 'flow'}
            <button role="menuitem" type="button" onclick={() => invoke(onfit)}>Fit graph</button>
          {:else}
            <button role="menuitemcheckbox" class="menu-toggle" type="button" aria-checked={performanceOpen} onclick={() => invoke(ontoggleperformance)}>
              <span class:checked={performanceOpen} aria-hidden="true"></span>
              Interaction timing
            </button>
          {/if}
        </div>
      {/if}
    </div>
  {/each}
</nav>
