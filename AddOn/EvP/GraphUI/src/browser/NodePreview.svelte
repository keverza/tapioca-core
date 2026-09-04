<script lang="ts">
  /**
   * The reserved right-hand column of the browser: what the highlighted node
   * looks like, and one sentence on what it does.
   *
   * ⚠️ THE PICTURE IS DRAWN FROM THE SCHEMA, NOT LOADED FROM A FILE. A folder of
   * per-node screenshots is a second catalog that nothing keeps in step: the day
   * a node grows a port, the picture stops being of that node and no test can
   * tell. This draws the same header colour, the same type-coloured nubs and the
   * same port names the real node on the canvas draws, from the one catalog the
   * runtime owns, so a node type added in C++ has a correct preview the moment
   * it appears in the list.
   */
  import { categoryColor, portColor } from '../nodes/types/display'
  import type { NodeTypeSchema } from '../types'

  let { schema }: { schema: NodeTypeSchema | undefined } = $props()
</script>

<aside class="preview" aria-label="Node preview" aria-live="polite">
  {#if schema === undefined}
    <p class="idle">Hover a node to preview it.</p>
  {:else}
    <div class="figure" style={`--node-color: ${categoryColor(schema.category)}`}>
      <div class="mini">
        <header>
          <span class="type-mark" aria-hidden="true"></span>
          <strong>{schema.label}</strong>
        </header>
        <div class="ports">
          <ul class="in">
            {#each schema.inputs as port}
              <li>
                <span class="nub" style={`--port-color: ${portColor(port.valueType)}`} aria-hidden="true"></span>
                <span class="port-label">{port.label}</span>
              </li>
            {:else}
              <li class="none">no inputs</li>
            {/each}
          </ul>
          <ul class="out">
            {#each schema.outputs as port}
              <li>
                <span class="port-label">{port.label}</span>
                <span class="nub" style={`--port-color: ${portColor(port.valueType)}`} aria-hidden="true"></span>
              </li>
            {:else}
              <li class="none">no outputs</li>
            {/each}
          </ul>
        </div>
      </div>
    </div>

    <h3>{schema.label}</h3>
    <span class="category">{schema.category}</span>
    <p class="description">{schema.description || 'This node type carries no description.'}</p>
    <dl>
      <dt>Type</dt>
      <dd>{schema.nodeType}</dd>
      <dt>Runs on</dt>
      <dd>{schema.executionDomain}</dd>
    </dl>
  {/if}
</aside>

<style>
  .preview { display: flex; overflow: hidden; flex-direction: column; padding: 9px 10px; border-left: 1px solid var(--border); background: color-mix(in srgb, var(--surface-raised) 45%, transparent); gap: 6px; }
  .idle { margin: auto 0; color: var(--text-faint); font-size: 10px; text-align: center; }

  .figure { display: grid; min-height: 96px; padding: 10px 6px; border: 1px solid var(--border); border-radius: 3px; background: var(--canvas); place-items: center; }
  .mini { width: 100%; max-width: 176px; overflow: hidden; border: 1px solid var(--border); border-radius: 3px; background: var(--surface); }
  .mini header { display: grid; align-items: center; padding: 0 6px; border-bottom: 1px solid var(--border); background: var(--surface-raised); gap: 5px; grid-template-columns: 8px minmax(0, 1fr); min-height: 20px; }
  .type-mark { width: 6px; height: 6px; transform: rotate(45deg); border: 1px solid var(--node-color); background: color-mix(in srgb, var(--node-color) 22%, transparent); }
  .mini header strong { overflow: hidden; color: var(--text); font-size: 9px; font-weight: 600; text-overflow: ellipsis; white-space: nowrap; }

  .ports { display: grid; padding: 5px 0; gap: 3px 8px; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); }
  ul { display: grid; margin: 0; padding: 0; list-style: none; gap: 3px; align-content: start; }
  li { display: flex; align-items: center; gap: 4px; }
  .out li { justify-content: flex-end; }
  .in li { margin-left: -4px; }
  .out li { margin-right: -4px; }
  .nub { width: 7px; height: 7px; flex: none; border: 1px solid var(--port-color); border-radius: 50%; background: color-mix(in srgb, var(--port-color) 55%, transparent); }
  .port-label { overflow: hidden; color: var(--text-muted); font-size: 8px; text-overflow: ellipsis; white-space: nowrap; }
  .none { color: var(--text-faint); font-size: 8px; font-style: italic; }

  h3 { margin: 2px 0 0; color: var(--text); font-size: 11px; font-weight: 600; }
  .category { color: var(--accent); font: 700 8px/1 ui-monospace, monospace; letter-spacing: 0.1em; text-transform: uppercase; }
  .description { overflow: hidden; margin: 0; color: var(--text-muted); font-size: 10px; line-height: 1.35; }
  dl { display: grid; margin: auto 0 0; padding-top: 6px; border-top: 1px solid var(--border); gap: 1px 6px; grid-template-columns: auto minmax(0, 1fr); }
  dt { color: var(--text-faint); font: 700 8px/1.4 ui-monospace, monospace; letter-spacing: 0.06em; text-transform: uppercase; }
  dd { overflow: hidden; margin: 0; color: var(--text-muted); font: 9px/1.4 ui-monospace, monospace; text-overflow: ellipsis; white-space: nowrap; }
</style>
