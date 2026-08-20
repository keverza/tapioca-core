import './styles.css'

type ValueKind = 'element' | 'geometry' | 'number' | 'result'
type RunStatus = 'idle' | 'queued' | 'running' | 'success' | 'error'

interface Binding {
  label: string
  value: string
  kind: ValueKind
}

interface CommandBlock {
  type: 'command'
  id: string
  title: string
  alias: string
  description: string
  bindings: Binding[]
  output: string
  outputKind: ValueKind
  status: RunStatus
  duration?: string
}

interface CommentBlock {
  type: 'comment'
  id: string
  text: string
}

type Block = CommandBlock | CommentBlock

interface NotebookSection {
  id: string
  title: string
  collapsed: boolean
  blocks: Block[]
}

const commandLibrary: Omit<CommandBlock, 'id' | 'status'>[] = [
  {
    type: 'command',
    title: 'Filter Elements',
    alias: 'filtered',
    description: 'Keep elements matching a property rule.',
    bindings: [
      { label: 'Elements', value: 'Workflow input / elements', kind: 'element' },
      { label: 'Rule', value: 'Layer is visible', kind: 'result' },
    ],
    output: 'elements',
    outputKind: 'element',
  },
  {
    type: 'command',
    title: 'Measure Path',
    alias: 'lengths',
    description: 'Measure positions along the selected path.',
    bindings: [
      { label: 'Path', value: 'Workflow input / path', kind: 'geometry' },
    ],
    output: 'distances',
    outputKind: 'number',
  },
  {
    type: 'command',
    title: 'Write Property',
    alias: 'updated',
    description: 'Write computed values to an Archicad property.',
    bindings: [
      { label: 'Elements', value: 'Previous command / elements', kind: 'element' },
      { label: 'Values', value: 'Previous command / values', kind: 'number' },
    ],
    output: 'modified_elements',
    outputKind: 'result',
  },
]

const sections: NotebookSection[] = [
  {
    id: 'prepare',
    title: 'Prepare geometry',
    collapsed: false,
    blocks: [
      {
        type: 'comment',
        id: 'intent',
        text: 'Use the one open polyline as the reference. Closed polylines remain elements to number.',
      },
      {
        type: 'command',
        id: 'selection',
        title: 'Read Selection',
        alias: 'selected',
        description: 'Classify one open reference polyline and the elements that will be numbered.',
        bindings: [
          { label: 'Selection', value: 'Current selection / 19 elements', kind: 'element' },
        ],
        output: 'reference + candidates',
        outputKind: 'element',
        status: 'idle',
      },
      {
        type: 'command',
        id: 'centers',
        title: 'Resolve Plan Centres',
        alias: 'centres',
        description: 'Use footprint centroids where available and batched 3D bounding boxes otherwise.',
        bindings: [
          { label: 'Candidates', value: 'selected / candidates', kind: 'element' },
          { label: 'Sample centre', value: '12.30, 4.75, 0.00', kind: 'geometry' },
        ],
        output: 'plan_centres',
        outputKind: 'geometry',
        status: 'idle',
      },
    ],
  },
  {
    id: 'assign',
    title: 'Order and number',
    collapsed: false,
    blocks: [
      {
        type: 'command',
        id: 'sorted',
        title: 'Sort Along Polyline',
        alias: 'ordered',
        description: 'Project each centre onto the open path and sort by arc length from its first vertex.',
        bindings: [
          { label: 'Elements', value: 'selected / candidates', kind: 'element' },
          { label: 'Centres', value: 'centres / plan_centres', kind: 'geometry' },
          { label: 'Direction', value: 'Nuo pradžios iki pabaigos', kind: 'result' },
        ],
        output: 'ordered_elements',
        outputKind: 'element',
        status: 'idle',
      },
      {
        type: 'command',
        id: 'numbers',
        title: 'Format Sequence',
        alias: 'numbers',
        description: 'Generate prefixed, zero-padded values using the requested first number.',
        bindings: [
          { label: 'Prefix', value: 'A-', kind: 'result' },
          { label: 'Format', value: '001, 002, 003', kind: 'result' },
          { label: 'Start', value: '1', kind: 'number' },
        ],
        output: 'A-001 ... A-018',
        outputKind: 'number',
        status: 'idle',
      },
      {
        type: 'command',
        id: 'result',
        title: 'Write Element IDs',
        alias: 'result',
        description: 'Write General_ElementID in one batched property call.',
        bindings: [
          { label: 'Elements', value: 'ordered / ordered_elements', kind: 'element' },
          { label: 'Values', value: 'numbers / formatted_values', kind: 'number' },
        ],
        output: '18 modified elements',
        outputKind: 'result',
        status: 'idle',
      },
    ],
  },
  {
    id: 'labels',
    title: 'Optional text labels',
    collapsed: false,
    blocks: [
      {
        type: 'command',
        id: 'labels-result',
        title: 'Create Text Labels',
        alias: 'labels',
        description: 'Place labels only on element kinds with a usable plan outline.',
        bindings: [
          { label: 'Placement', value: 'shortest edge', kind: 'result' },
          { label: 'Alignment', value: 'centre', kind: 'result' },
          { label: 'Edge offset', value: '0.000', kind: 'number' },
          { label: 'Text height', value: '2.500', kind: 'number' },
          { label: 'Layer', value: 'Annotation', kind: 'result' },
        ],
        output: 'placed_text',
        outputKind: 'result',
        status: 'idle',
      },
    ],
  },
]

const appRoot = document.querySelector<HTMLElement>('#app')
if (appRoot === null) throw new Error('Notebook root element is missing')
const app: HTMLElement = appRoot

let pathState: 'missing' | 'picked' | 'current' | 'internalized' = 'missing'
let outlineOpen = false
let addDialogOpen = false
let addQuery = ''
let outlineQuery = ''
let runToken = 0
let isRunning = false
let forceFailure = false
let highlightId = ''
let nextBlockId = 1
let draggedBlockId = ''

const icon = (name: 'menu' | 'search' | 'chevron' | 'plus' | 'close' | 'block' | 'drag'): string => {
  const paths = {
    menu: '<path d="M4 7h16M4 12h16M4 17h16"/>',
    search: '<circle cx="11" cy="11" r="6"/><path d="m16 16 4 4"/>',
    chevron: '<path d="m9 6 6 6-6 6"/>',
    plus: '<path d="M12 5v14M5 12h14"/>',
    close: '<path d="m6 6 12 12M18 6 6 18"/>',
    block: '<path d="m12 3 7 4-7 4-7-4 7-4Z"/><path d="m5 7v9l7 5 7-5V7M12 11v10"/>',
    drag: '<circle cx="9" cy="7" r=".7"/><circle cx="15" cy="7" r=".7"/><circle cx="9" cy="12" r=".7"/><circle cx="15" cy="12" r=".7"/><circle cx="9" cy="17" r=".7"/><circle cx="15" cy="17" r=".7"/>',
  }
  return `<svg aria-hidden="true" viewBox="0 0 24 24">${paths[name]}</svg>`
}

const commandBlocks = (): CommandBlock[] => sections.flatMap((section) =>
  section.blocks.filter((block): block is CommandBlock => block.type === 'command'),
)

const validationMessage = (): string => pathState === 'missing'
  ? '1 required reference needs attention'
  : 'Ready to run'

const statusLabel = (block: CommandBlock): string => {
  if (block.status === 'running') return 'Running'
  if (block.status === 'queued') return 'Queued'
  if (block.status === 'success') return `Complete / ${block.duration}`
  if (block.status === 'error') return 'Failed'
  return 'Not run'
}

function renderOutline(): string {
  const query = outlineQuery.trim().toLowerCase()
  const entries = sections.flatMap((section) => [
    { id: section.id, label: section.title, type: 'section' },
    ...section.blocks
      .filter((block): block is CommandBlock => block.type === 'command')
      .map((block) => ({ id: block.id, label: block.alias, type: block.title })),
  ]).filter((entry) => `${entry.label} ${entry.type}`.toLowerCase().includes(query))

  return `
    <aside class="outline ${outlineOpen ? 'is-open' : ''}" aria-label="Notebook outline">
      <div class="outline-head">
        <strong>Outline</strong>
        <button class="icon-button outline-close" data-action="close-outline" aria-label="Close outline" title="Close outline">${icon('close')}</button>
      </div>
      <label class="search-field">
        <span class="sr-only">Search notebook</span>
        ${icon('search')}
        <input id="outline-search" type="search" value="${escapeAttribute(outlineQuery)}" placeholder="Find blocks" autocomplete="off" />
        <kbd>Ctrl F</kbd>
      </label>
      <nav class="outline-list" aria-label="Notebook contents">
        ${entries.length === 0 ? '<p class="empty-state">No matching blocks</p>' : entries.map((entry) => `
          <button class="outline-entry ${entry.type === 'section' ? 'section-entry' : ''}" data-action="jump" data-id="${entry.id}">
            <span>${escapeHtml(entry.label)}</span>
            <small>${escapeHtml(entry.type)}</small>
          </button>
        `).join('')}
      </nav>
    </aside>
    <button class="outline-scrim ${outlineOpen ? 'is-open' : ''}" data-action="close-outline" aria-label="Close outline"></button>
  `
}

function renderBinding(binding: Binding): string {
  const value = binding.kind === 'geometry'
    ? `<span class="vector-value" aria-label="${escapeAttribute(binding.label)} vector"><input value="12.30" aria-label="X" /><input value="4.75" aria-label="Y" /><input value="0.00" aria-label="Z" /><small>m</small></span>`
    : binding.kind === 'number'
      ? `<span class="scalar-value"><input value="${escapeAttribute(binding.value)}" aria-label="${escapeAttribute(binding.label)}" /><small>${binding.label === 'Edge offset' ? 'm' : binding.label === 'Text height' ? 'mm' : ''}</small></span>`
      : `<span class="binding-value">${escapeHtml(binding.value)}</span>`
  return `
    <div class="binding-row">
      <span class="binding-label">${escapeHtml(binding.label)}</span>
      ${value}
    </div>
  `
}

function renderCommand(block: CommandBlock, sectionId: string): string {
  return `
    <article id="${block.id}" class="command-card kind-${block.outputKind} ${block.status} ${highlightId === block.id ? 'highlight' : ''}"
      tabindex="0" draggable="true" data-command-id="${block.id}" data-section-id="${sectionId}">
      <header class="command-head">
        <span class="drag-handle" aria-label="Drag ${escapeAttribute(block.alias)} to reorder" title="Drag to reorder">${icon('drag')}</span>
        <span class="block-glyph" aria-hidden="true">${icon('block')}</span>
        <div class="object-name">
          <input id="alias-${block.id}" class="alias-input" value="${escapeAttribute(block.alias)}" data-action="alias" data-id="${block.id}"
            aria-label="Object name for ${escapeAttribute(block.title)}" spellcheck="false" />
        </div>
        <span class="block-name">${escapeHtml(block.title)}</span>
        <div class="block-indicators">
          <button class="indicator help" aria-label="Help for ${escapeAttribute(block.title)}" title="Block help">?</button>
          <span class="indicator state ${block.status}" aria-label="${escapeAttribute(statusLabel(block))}" title="${escapeAttribute(statusLabel(block))}">!</span>
          ${block.duration ? `<small>${block.duration}</small>` : ''}
        </div>
      </header>
      <div class="command-body">
        <div class="bindings">${block.bindings.map(renderBinding).join('')}</div>
        <div class="output-row">
          <span>Output</span>
          <strong>${escapeHtml(block.output)}</strong>
        </div>
        ${block.id === 'result' ? `
          <label class="failure-toggle">
            <input type="checkbox" id="force-failure" ${forceFailure ? 'checked' : ''} />
            Simulate write failure on next run
          </label>
        ` : ''}
        ${block.status === 'error' ? '<p class="inline-error" role="alert">Element could not be modified. Clear the simulated failure and run again.</p>' : ''}
      </div>
    </article>
  `
}

function renderSection(section: NotebookSection): string {
  return `
    <section id="${section.id}" class="notebook-section ${highlightId === section.id ? 'highlight' : ''}">
      <button class="section-title" data-action="toggle-section" data-id="${section.id}" aria-expanded="${!section.collapsed}">
        <span class="disclosure ${section.collapsed ? '' : 'expanded'}">${icon('chevron')}</span>
        <span>${escapeHtml(section.title)}</span>
        <small>${section.blocks.filter((block) => block.type === 'command').length} commands</small>
      </button>
      ${section.collapsed ? '' : `
        <div class="section-content">
          ${section.blocks.map((block) => block.type === 'comment'
            ? `<aside id="${block.id}" class="comment-block"><span>Note</span><p>${escapeHtml(block.text)}</p></aside>`
            : renderCommand(block, section.id)).join('')}
          <div class="section-drop-zone" data-drop-section="${section.id}" aria-hidden="true">Drop block here</div>
        </div>
      `}
    </section>
  `
}

function renderAddDialog(): string {
  if (!addDialogOpen) return ''
  const query = addQuery.trim().toLowerCase()
  const matches = commandLibrary.filter((command) =>
    `${command.title} ${command.description}`.toLowerCase().includes(query),
  )
  return `
    <div class="modal-layer" role="presentation">
      <section class="command-dialog" role="dialog" aria-modal="true" aria-labelledby="add-title">
        <header>
          <div><p class="eyebrow">Command library</p><h2 id="add-title">Add command</h2></div>
          <button class="icon-button" data-action="close-add" aria-label="Close command dialog" title="Close">${icon('close')}</button>
        </header>
        <label class="search-field dialog-search">
          <span class="sr-only">Search commands</span>${icon('search')}
          <input id="add-search" type="search" value="${escapeAttribute(addQuery)}" placeholder="Search commands" autocomplete="off" />
        </label>
        <div class="command-results">
          ${matches.length === 0 ? '<p class="empty-state">No compatible commands found</p>' : matches.map((command) => `
            <button data-action="add-command" data-library-index="${commandLibrary.indexOf(command)}">
              <span><strong>${escapeHtml(command.title)}</strong><small>${escapeHtml(command.description)}</small></span>
              ${icon('plus')}
            </button>
          `).join('')}
        </div>
      </section>
    </div>
  `
}

function render(): void {
  const focused = document.activeElement instanceof HTMLElement
    ? { action: document.activeElement.dataset.action, id: document.activeElement.dataset.id, elementId: document.activeElement.id }
    : null
  app.innerHTML = `
    <div class="app-shell">
      ${renderOutline()}
      <main class="workspace">
        <header class="action-bar">
          <button class="icon-button outline-toggle" data-action="open-outline" aria-label="Open outline" title="Open outline">${icon('menu')}</button>
          <div class="action-title"><strong>Number Elements Along Polyline</strong><span class="dirty-dot">Edited</span></div>
          <span class="validation ${pathState === 'missing' ? 'warning' : 'valid'}">${validationMessage()}</span>
          <div class="run-actions">
            <button class="button secondary" data-action="cancel" ${isRunning ? '' : 'disabled'}>Cancel</button>
            <button class="button primary" data-action="run" ${pathState === 'missing' || isRunning ? 'disabled' : ''}>${isRunning ? 'Running' : 'Run'}</button>
          </div>
        </header>

        <div class="document-scroll">
          <article class="notebook-document">
            <header class="document-header">
              <p class="eyebrow">Workflow notebook</p>
              <h1>Number Elements Along Polyline</h1>
              <p>Order selected elements along a reference path and assign sequential element IDs.</p>
              <div class="document-meta"><span>${commandBlocks().length} blocks</span><span>Explicit execution</span><span>Local mock</span></div>
            </header>

            <section class="workflow-inputs" aria-labelledby="inputs-title">
              <div class="region-title"><div><p class="eyebrow">Configuration</p><h2 id="inputs-title">Workflow inputs</h2></div><span class="validation ${pathState === 'missing' ? 'warning' : 'valid'}">${validationMessage()}</span></div>
              <div class="input-grid">
                <article class="input-card">
                  <div><span class="type-chip element">Elements</span><strong>elements</strong></div>
                  <p>Current selection / 1 open polyline + 18 elements</p>
                  <button class="button secondary compact">Refresh selection</button>
                </article>
                <article class="input-card ${pathState === 'missing' ? 'missing' : ''}">
                  <div><span class="type-chip geometry">Polyline</span><strong>path</strong></div>
                  <p>${pathState === 'missing' ? 'Missing reference / Main Numbering Path' : pathState === 'picked' ? 'Picked / Main Numbering Path' : pathState === 'current' ? 'Current selection / Polyline' : 'Internalized / Main Numbering Path'}</p>
                  ${pathState === 'missing' ? '<span class="warning-copy">Required path is unavailable</span>' : ''}
                  <div class="reference-actions">
                    <button class="button secondary compact" data-action="set-path" data-value="picked">Pick</button>
                    <button class="button secondary compact" data-action="set-path" data-value="current">Current</button>
                    <button class="button secondary compact" data-action="set-path" data-value="internalized">Internalize</button>
                    <button class="button ghost compact" data-action="set-path" data-value="missing">Break reference</button>
                  </div>
                </article>
              </div>
            </section>

            <div class="stack-heading"><div><p class="eyebrow">Notebook</p><h2>Command stack</h2></div><span>Top to bottom</span></div>
            ${sections.map(renderSection).join('')}
            <button class="add-command" data-action="open-add">${icon('plus')}<span>Add command</span><kbd>Ctrl K</kbd></button>

            <section class="workflow-output" aria-labelledby="output-title">
              <div><p class="eyebrow">Workflow output</p><h2 id="output-title">Result</h2></div>
              <span class="type-chip result">result / 18 modified elements + placed text</span>
            </section>
          </article>
        </div>
      </main>
      ${renderAddDialog()}
    </div>
  `
  restoreFocus(focused)
}

function restoreFocus(focused: { action?: string; id?: string; elementId: string } | null): void {
  if (addDialogOpen) {
    document.querySelector<HTMLInputElement>('#add-search')?.focus()
    return
  }
  if (focused?.elementId === 'outline-search') {
    const input = document.querySelector<HTMLInputElement>('#outline-search')
    input?.focus()
    input?.setSelectionRange(input.value.length, input.value.length)
    return
  }
  if (focused?.action) {
    document.querySelector<HTMLElement>(`[data-action="${focused.action}"][data-id="${focused.id ?? ''}"]`)?.focus()
  }
}

function escapeHtml(value: string): string {
  return value.replace(/[&<>"']/g, (character) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#039;',
  })[character] ?? character)
}

function escapeAttribute(value: string): string {
  return escapeHtml(value)
}

function findBlock(id: string): { section: NotebookSection; block: Block; index: number } | null {
  for (const section of sections) {
    const index = section.blocks.findIndex((block) => block.id === id)
    if (index >= 0) return { section, block: section.blocks[index], index }
  }
  return null
}

function jumpTo(id: string): void {
  const section = sections.find((item) => item.id === id)
  if (section) section.collapsed = false
  const block = findBlock(id)
  if (block) block.section.collapsed = false
  highlightId = id
  outlineOpen = false
  render()
  requestAnimationFrame(() => {
    document.getElementById(id)?.scrollIntoView({ behavior: 'smooth', block: 'center' })
    window.setTimeout(() => {
      highlightId = ''
      document.getElementById(id)?.classList.remove('highlight')
    }, 1200)
  })
}

const wait = (milliseconds: number): Promise<void> => new Promise((resolve) => window.setTimeout(resolve, milliseconds))

async function runWorkflow(): Promise<void> {
  if (pathState === 'missing' || isRunning) return
  const token = ++runToken
  isRunning = true
  commandBlocks().forEach((block) => {
    block.status = 'queued'
    block.duration = undefined
  })
  render()

  const blocks = commandBlocks()
  for (let index = 0; index < blocks.length; index += 1) {
    if (token !== runToken) return
    const block = blocks[index]
    block.status = 'running'
    render()
    await wait(420)
    if (token !== runToken) return
    if (forceFailure && block.id === 'result') {
      block.status = 'error'
      isRunning = false
      render()
      return
    }
    block.status = 'success'
    block.duration = ['15 ms', '8 ms', '<1 ms', '31 ms'][index] ?? '4 ms'
    render()
    await wait(120)
  }
  isRunning = false
  render()
}

function cancelWorkflow(): void {
  if (!isRunning) return
  runToken += 1
  isRunning = false
  commandBlocks().forEach((block) => {
    if (block.status === 'running' || block.status === 'queued') block.status = 'idle'
  })
  render()
}

app.addEventListener('click', (event) => {
  const target = (event.target as HTMLElement).closest<HTMLElement>('[data-action]')
  if (!target) return
  const action = target.dataset.action
  const id = target.dataset.id ?? ''

  if (action === 'open-outline') { outlineOpen = true; render(); document.querySelector<HTMLInputElement>('#outline-search')?.focus() }
  if (action === 'close-outline') { outlineOpen = false; render() }
  if (action === 'jump') jumpTo(id)
  if (action === 'toggle-section') {
    const section = sections.find((item) => item.id === id)
    if (section) {
      const beforeY = target.getBoundingClientRect ().top
      section.collapsed = !section.collapsed
      render()
      const afterY = document.querySelector<HTMLElement>(`[data-action="toggle-section"][data-id="${id}"]`)?.getBoundingClientRect ().top
      if (afterY !== undefined) document.querySelector<HTMLElement>('.document-scroll')?.scrollBy (0, afterY - beforeY)
    }
  }
  if (action === 'set-path') { pathState = target.dataset.value as typeof pathState; render() }
  if (action === 'open-add') { addDialogOpen = true; addQuery = ''; render() }
  if (action === 'close-add') { addDialogOpen = false; render() }
  if (action === 'add-command') {
    const template = commandLibrary[Number(target.dataset.libraryIndex)]
    const newBlock: CommandBlock = { ...template, id: `added-${nextBlockId++}`, status: 'idle', bindings: template.bindings.map((binding) => ({ ...binding })) }
    sections[sections.length - 1].blocks.push(newBlock)
    addDialogOpen = false
    render()
    jumpTo(newBlock.id)
  }
  if (action === 'run') void runWorkflow()
  if (action === 'cancel') cancelWorkflow()
})

app.addEventListener('input', (event) => {
  const target = event.target
  if (!(target instanceof HTMLInputElement)) return
  if (target.id === 'outline-search') { outlineQuery = target.value; render() }
  if (target.id === 'add-search') { addQuery = target.value; render() }
  if (target.id === 'force-failure') { forceFailure = target.checked }
  if (target.dataset.action === 'alias') {
    const found = findBlock(target.dataset.id ?? '')
    if (found?.block.type === 'command') found.block.alias = target.value
  }
})

app.addEventListener('change', (event) => {
  const target = event.target
  if (target instanceof HTMLInputElement && target.dataset.action === 'alias') render()
})

app.addEventListener('dragstart', (event) => {
  const card = (event.target as HTMLElement).closest<HTMLElement>('[data-command-id]')
  if (!card || (event.target as HTMLElement).closest('input, button')) {
    event.preventDefault()
    return
  }
  draggedBlockId = card.dataset.commandId ?? ''
  card.classList.add('dragging')
  event.dataTransfer?.setData('text/plain', draggedBlockId)
  if (event.dataTransfer) event.dataTransfer.effectAllowed = 'move'
})

app.addEventListener('dragover', (event) => {
  if (!draggedBlockId) return
  const card = (event.target as HTMLElement).closest<HTMLElement>('[data-command-id]')
  const zone = (event.target as HTMLElement).closest<HTMLElement>('[data-drop-section]')
  if (!card && !zone) return
  event.preventDefault()
  document.querySelectorAll('.drop-before, .drop-end').forEach((item) => item.classList.remove('drop-before', 'drop-end'))
  if (card && card.dataset.commandId !== draggedBlockId) card.classList.add('drop-before')
  if (zone) zone.classList.add('drop-end')
})

app.addEventListener('drop', (event) => {
  if (!draggedBlockId) return
  const targetCard = (event.target as HTMLElement).closest<HTMLElement>('[data-command-id]')
  const zone = (event.target as HTMLElement).closest<HTMLElement>('[data-drop-section]')
  const source = findBlock(draggedBlockId)
  const targetSectionId = targetCard?.dataset.sectionId ?? zone?.dataset.dropSection
  const targetSection = sections.find((section) => section.id === targetSectionId)
  if (!source || !targetSection || source.block.type !== 'command') return
  event.preventDefault()
  source.section.blocks.splice(source.index, 1)
  let targetIndex = targetCard ? targetSection.blocks.findIndex((block) => block.id === targetCard.dataset.commandId) : targetSection.blocks.length
  if (targetIndex < 0) targetIndex = targetSection.blocks.length
  targetSection.blocks.splice(targetIndex, 0, source.block)
  draggedBlockId = ''
  render()
})

app.addEventListener('dragend', () => {
  draggedBlockId = ''
  document.querySelectorAll('.dragging, .drop-before, .drop-end').forEach((item) => item.classList.remove('dragging', 'drop-before', 'drop-end'))
})

document.addEventListener('keydown', (event) => {
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'f') {
    event.preventDefault()
    outlineOpen = true
    render()
    document.querySelector<HTMLInputElement>('#outline-search')?.focus()
  }
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'k') {
    event.preventDefault()
    addDialogOpen = true
    addQuery = ''
    render()
  }
  if (event.key === 'Escape') {
    if (addDialogOpen) addDialogOpen = false
    else outlineOpen = false
    render()
  }
})

render()
