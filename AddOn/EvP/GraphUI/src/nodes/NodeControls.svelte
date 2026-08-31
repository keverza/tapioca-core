<script lang="ts">
  import type { GraphParameter, NodeOutputRecord, NodeTypeSchema } from '../types'
  import NodePort from './NodePort.svelte'

  let { schema, parameters, outputs }: { schema: NodeTypeSchema; parameters: GraphParameter[]; outputs?: NodeOutputRecord[] } = $props()
  function parameterText(parameter?: GraphParameter): string {
    if (parameter?.value?.text !== undefined) return parameter.value.text
    if (parameter?.value?.number !== undefined) return String(parameter.value.number)
    if (parameter?.numberValue !== undefined) return String(parameter.numberValue)
    if (parameter?.value?.bool !== undefined) return parameter.value.bool ? 'True' : 'False'
    return '-'
  }
  function outputText(portId: string): string | undefined { return outputs?.find((output) => output.portId === portId)?.summary }
</script>

<section class="controls nodrag">
  {#each schema.inputs as input, index}
    <div class="row">
      <NodePort port={input} direction="input" />
      <output>{parameterText(parameters.find((item) => item.parameterId === input.portId) ?? parameters[index])}</output>
      <small>{input.valueType}</small>
    </div>
  {/each}
  {#each schema.parameters.filter((parameter) => !schema.inputs.some((input) => input.portId === parameter.parameterId)) as parameter}
    <div class="row internal">
      <span>{parameter.label}</span>
      <output>{parameterText(parameters.find((item) => item.parameterId === parameter.parameterId))}</output>
      <small>{parameter.valueType}</small>
    </div>
  {/each}
</section>
<section class="outputs">
  {#each schema.outputs as output}<NodePort port={output} direction="output" value={outputText(output.portId)} />{/each}
</section>

<style>
  .controls { padding: 6px 8px 5px 11px; }
  .row { display: grid; grid-template-columns: minmax(70px, 1fr) minmax(58px, .8fr) 38px; align-items: center; min-height: 25px; border-bottom: 1px solid color-mix(in srgb, var(--border) 55%, transparent); }
  .row.internal { padding-left: 2px; }
  .row > span { overflow: hidden; color: var(--text-muted); font-size: 9px; text-overflow: ellipsis; white-space: nowrap; }
  output { overflow: hidden; min-height: 19px; padding: 4px 5px; background: var(--canvas); color: var(--text); font: 9px/1.2 ui-monospace, monospace; text-overflow: ellipsis; white-space: nowrap; }
  small { padding-left: 5px; overflow: hidden; color: var(--text-faint); font: 7px/1 ui-monospace, monospace; text-overflow: ellipsis; }
  .outputs { display: grid; justify-content: end; padding: 3px 10px 7px; border-top: 1px solid var(--border); }
</style>
