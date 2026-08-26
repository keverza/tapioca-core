<script lang="ts">
  import { Handle, Position, type Node, type NodeProps } from '@xyflow/svelte'
  import type { SchemaNodeData } from './types'

  let { data, selected }: NodeProps<Node<SchemaNodeData>> = $props()
</script>

<article class:selected class:error={data.result?.status === 'failed' || data.result?.status === 'blocked'}>
  <header>
    <span>{data.schema.category}</span>
    <strong>{data.schema.label}</strong>
  </header>

  <p>{data.schema.description}</p>

  {#if data.parameters.length > 0}
    <dl>
      {#each data.parameters as parameter}
        <div>
          <dt>{parameter.parameterId}</dt>
          <dd>{parameter.numberValue ?? '-'}</dd>
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
            style={`top: ${92 + index * 24}px`}
          />
          <span>{input.label}</span>
          <small>{input.valueType}</small>
        </div>
      {/each}
    </div>
    <div>
      {#each data.schema.outputs as output, index}
        <div class="port output">
          <small>{output.valueType}</small>
          <span>{output.label}</span>
          <Handle
            type="source"
            position={Position.Right}
            id={output.portId}
            style={`top: ${92 + index * 24}px`}
          />
        </div>
      {/each}
    </div>
  </div>

  <footer class:complete={data.result?.status === 'complete'}>
    <span>{data.result?.status ?? 'not run'}</span>
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
    border: 1px solid #29313b;
    border-radius: 5px;
    background: #151a20;
    color: #e8edf2;
    box-shadow: 0 8px 24px rgb(0 0 0 / 30%);
  }

  article.selected {
    border-color: #ffb000;
    box-shadow: 0 0 0 1px #ffb000, 0 10px 28px rgb(0 0 0 / 38%);
  }

  article.error {
    border-color: #e36d5b;
  }

  header {
    display: grid;
    gap: 2px;
    padding: 9px 12px;
    border-bottom: 1px solid #303945;
    background: #202731;
  }

  header span {
    color: #ffb000;
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
    color: #9aa8b7;
    font: 11px/1.35 'Segoe UI', sans-serif;
  }

  dl {
    margin: 0 12px 8px;
    border: 1px solid #2a333e;
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
    color: #ffcb5b;
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
    color: #718092;
    font-size: 8px;
    text-transform: uppercase;
  }

  .output {
    justify-content: flex-end;
    text-align: right;
  }

  footer {
    display: flex;
    justify-content: space-between;
    gap: 8px;
    padding: 6px 10px;
    background: #101419;
    color: #718092;
    font: 9px/1.2 ui-monospace, monospace;
  }

  footer.complete {
    color: #75c695;
  }

  :global(.svelte-flow__handle) {
    width: 8px;
    height: 8px;
    border: 1px solid #151a20;
    background: #ffb000;
  }
</style>
