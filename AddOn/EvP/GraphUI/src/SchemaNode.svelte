<script lang="ts">
  import { Handle, Position, type Node, type NodeProps } from '@xyflow/svelte'
  import ThreltePreview from './ThreltePreview.svelte'
  import type { NodeOutputRecord, SchemaNodeData } from './types'

  let { data, selected }: NodeProps<Node<SchemaNodeData>> = $props()

  const HEADER_OFFSET = 92
  const PORT_PITCH = 24

  /** What the runtime rendered for one output port, if it has run. */
  function outputFor(portId: string): NodeOutputRecord | undefined {
    return data.result?.outputs?.find((output) => output.portId === portId)
  }

  /**
   * The panel body. The runtime already formatted the value - including
   * truncation and its "... N more" line - so this splits rather than
   * re-rendering, and what a panel shows cannot drift from what the runtime
   * says the value is.
   */
  const panelLines = $derived.by((): string[] => {
    const text = outputFor('text')?.text
    if (text === undefined || text === '') return []
    return text.split('\n')
  })

  const hasRun = $derived(
    data.result !== undefined && data.result.status !== 'dirty' && data.result.outputs !== undefined,
  )

  function parameterText(parameter: { value?: { text?: string; number?: number }; numberValue?: number }): string {
    if (parameter.value?.text !== undefined) return parameter.value.text
    if (parameter.value?.number !== undefined) return String(parameter.value.number)
    if (parameter.numberValue !== undefined) return String(parameter.numberValue)
    return '-'
  }
</script>

<article
  class:selected
  class:error={data.result?.status === 'failed' || data.result?.status === 'blocked'}
  class:skipped={data.result?.status === 'skipped'}
>
  <header>
    <span>{data.schema.category}</span>
    <strong>{data.schema.label}</strong>
  </header>

  <p class="schema-description">{data.schema.description}</p>

  {#if data.parameters.length > 0}
    <dl class="schema-parameters">
      {#each data.parameters as parameter}
        <div>
          <dt>{parameter.parameterId}</dt>
          <dd>{parameterText(parameter)}</dd>
        </div>
      {/each}
    </dl>
  {/if}

  <div class="ports">
    <div>
      {#each data.schema.inputs as input, index}
        <div class="port input">
          <Handle
            type="target"
            position={Position.Left}
            id={input.portId}
            style={`top: ${HEADER_OFFSET + index * PORT_PITCH}px`}
          />
          <span>{input.label}</span>
          <small class="port-type">{input.valueType === 'absent' ? 'any' : input.valueType}</small>
        </div>
      {/each}
    </div>
    <div>
      {#each data.schema.outputs as output, index}
        <div class="port output">
          <!-- The value, not just the type. Before this, a node could run
               successfully and leave nothing on screen to look at. -->
          {#if outputFor(output.portId) !== undefined}
            <small class="value port-value" title={outputFor(output.portId)?.text}>
              {outputFor(output.portId)?.summary}
            </small>
          {:else}
            <small class="port-type">{output.valueType}</small>
          {/if}
          <span>{output.label}</span>
          <Handle
            type="source"
            position={Position.Right}
            id={output.portId}
            style={`top: ${HEADER_OFFSET + index * PORT_PITCH}px`}
          />
        </div>
      {/each}
    </div>
  </div>

  {#if data.schema.display === 'text'}
    <!-- The Grasshopper-panel body. nodrag/nowheel so selecting and scrolling
         the text does not pan the canvas underneath it. -->
    <section class="panel nodrag nowheel" aria-label="Node output">
      {#if !hasRun}
        <p class="empty">Not evaluated yet</p>
      {:else if panelLines.length === 0}
        <p class="empty">(nothing)</p>
      {:else}
        <ol>
          {#each panelLines as line, index}
            <li><span class="index">{index + 1}</span><span class="line">{line}</span></li>
          {/each}
        </ol>
      {/if}
      {#if outputFor('summary')?.text}
        <footer class="panel-summary">{outputFor('summary')?.text}</footer>
      {/if}
    </section>
  {:else if data.schema.display === 'preview'}
    {#if selected}
      <ThreltePreview result={data.result} />
    {:else}
      <div class="preview-hint">Select to activate 3D preview</div>
    {/if}
  {/if}

  <footer class:complete={data.result?.status === 'complete'}>
    <span>{data.result?.status ?? 'not run'}{data.result?.cacheHit ? ' (cached)' : ''}</span>
    {#if data.result?.status === 'complete'}
      <span>{data.result.durationMilliseconds.toFixed(2)} ms / {data.result.itemCount} items</span>
    {:else if data.result?.message}
      <span>{data.result.message}</span>
    {/if}
  </footer>
</article>

<style>
  article {
    width: 228px;
    overflow: hidden;
    border: 1px solid var(--border);
    border-radius: 5px;
    background: var(--surface);
    color: var(--text);
    box-shadow: 0 8px 24px rgb(0 0 0 / 30%);
  }

  article.selected {
    border-color: var(--accent);
    box-shadow: 0 0 0 1px var(--accent), 0 10px 28px rgb(0 0 0 / 38%);
  }

  article.error {
    border-color: var(--danger);
  }

  article.skipped {
    border-color: #6a7482;
  }

  header {
    display: grid;
    gap: 2px;
    padding: 9px 12px;
    border-bottom: 1px solid var(--border);
    background: var(--surface-raised);
  }

  header span {
    color: var(--accent);
    font: 600 9px/1.2 'Segoe UI', sans-serif;
    letter-spacing: 0.14em;
    text-transform: uppercase;
  }

  header strong {
    font: 600 14px/1.25 'Segoe UI', sans-serif;
  }

  p {
    min-height: 30px;
    margin: 0;
    padding: 8px 12px;
    color: var(--text-muted);
    font: 11px/1.35 'Segoe UI', sans-serif;
  }

  dl {
    margin: 0 12px 8px;
    border: 1px solid var(--border);
    border-radius: 3px;
  }

  dl div,
  .ports {
    display: flex;
    justify-content: space-between;
  }

  dt,
  dd {
    margin: 0;
    padding: 4px 6px;
    font: 10px/1.2 ui-monospace, monospace;
  }

  dd {
    color: var(--accent-soft);
  }

  .ports {
    gap: 14px;
    padding: 2px 12px 9px;
  }

  .ports > div {
    display: grid;
    gap: 8px;
  }

  .port {
    display: flex;
    gap: 5px;
    align-items: baseline;
    min-height: 16px;
    font: 10px/1.2 'Segoe UI', sans-serif;
  }

  .port small {
    color: var(--text-faint);
    font-size: 8px;
    text-transform: uppercase;
  }

  .port small.value {
    max-width: 96px;
    overflow: hidden;
    color: var(--accent-soft);
    font: 9px/1.2 ui-monospace, monospace;
    text-overflow: ellipsis;
    text-transform: none;
    white-space: nowrap;
  }

  .output {
    justify-content: flex-end;
    text-align: right;
  }

  .panel {
    margin: 0 10px 10px;
    overflow: hidden;
    border: 1px solid var(--border);
    border-radius: 4px;
    background: var(--canvas);
  }

  .panel ol {
    max-height: 168px;
    margin: 0;
    padding: 4px 0;
    overflow: auto;
    list-style: none;
  }

  .panel li {
    display: flex;
    gap: 8px;
    padding: 1px 8px;
    font: 10px/1.45 ui-monospace, monospace;
  }

  .panel li:nth-child(even) {
    background: color-mix(in srgb, var(--surface) 72%, var(--canvas));
  }

  .panel .index {
    flex: none;
    min-width: 16px;
    color: var(--text-faint);
    text-align: right;
    user-select: none;
  }

  .panel .line {
    color: var(--text);
    white-space: pre-wrap;
    word-break: break-word;
  }

  .panel .empty {
    min-height: 0;
    padding: 10px 8px;
    color: var(--text-faint);
    font: 10px/1.2 ui-monospace, monospace;
    text-align: center;
  }

  .panel-summary {
    padding: 4px 8px;
    border-top: 1px solid var(--border);
    color: var(--text-faint);
    font: 9px/1.2 ui-monospace, monospace;
  }

  footer {
    display: flex;
    justify-content: space-between;
    gap: 8px;
    padding: 6px 10px;
    background: var(--canvas);
    color: var(--text-faint);
    font: 9px/1.2 ui-monospace, monospace;
  }

  footer.complete {
    color: #75c695;
  }

  .preview-hint {
    margin: 0 10px 10px;
    padding: 9px;
    border: 1px dashed var(--border);
    border-radius: 4px;
    color: var(--text-faint);
    font: 9px/1.2 ui-monospace, monospace;
    text-align: center;
  }

  :global(.svelte-flow__handle) {
    width: 8px;
    height: 8px;
    border: 1px solid var(--surface);
    background: var(--accent);
  }
</style>
