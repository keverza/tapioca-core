<script lang="ts">
  import type { GraphParameter, NodeOutputRecord } from '../types'
  import type { NodeDefinition } from './types/node'
  import type { PortConnectionState, PortLayout } from './types/port'
  import type { ComponentMessage } from './types/diagnostics'
  import { parsePortReference, type PortReference } from './types/portReference'
  import NodePort from './NodePort.svelte'

  let { nodeId, definition, parameters, outputs, layout, connections, messages = [], onparameter, onreference, onportmenu }: { nodeId: string; definition: NodeDefinition; parameters: GraphParameter[]; outputs?: NodeOutputRecord[]; layout: PortLayout; connections: PortConnectionState[]; messages?: ComponentMessage[]; onparameter?: (nodeId: string, parameterId: string, valueType: string, text: string) => void; onreference?: (reference: PortReference, targetPortId: string) => void; onportmenu?: (event: MouseEvent, portId: string, direction: 'input' | 'output') => void } = $props()

  function parameterText(parameter?: GraphParameter): string {
    if (parameter?.value?.text !== undefined) return parameter.value.text
    if (parameter?.value?.numbers !== undefined) return parameter.value.numbers.join(', ')
    if (parameter?.value?.number !== undefined) return String(parameter.value.number)
    if (parameter?.numberValue !== undefined) return String(parameter.numberValue)
    if (parameter?.value?.bool !== undefined) return parameter.value.bool ? 'True' : 'False'
    return ''
  }
  function outputText(portId: string): string | undefined { return outputs?.find((output) => output.portId === portId)?.summary }
  function stored(parameterId: string): GraphParameter | undefined {
    return parameters.find((item) => item.parameterId === parameterId)
  }
  function isConnected(portId: string): boolean {
    return connections.some((item) => item.portId === portId && item.direction === 'input' && item.connected)
  }
  /** What a connected input is showing: the upstream value, which is not editable here. */
  function upstreamText(portId: string): string {
    const peer = connections.find((item) => item.portId === portId && item.direction === 'input')?.peerLabels?.[0]
    return peer ?? 'connected'
  }

  /**
   * Blur commits - but only a CHANGE commits. Every setParam is a document edit
   * that bumps the revision and dirties everything downstream of the node, so
   * merely clicking away from a box the user only looked at must not cost a
   * re-evaluation.
   */
  function commit(event: Event, parameterId: string, valueType: string): void {
    const field = event.currentTarget as HTMLInputElement
    if (field.value.trim() === parameterText(stored(parameterId)).trim()) return
    onparameter?.(nodeId, parameterId, valueType, field.value)
  }

  /**
   * A paste into the box is either a value or a WIRE. The clipboard carries the
   * port menu's own reference format, so a pasted reference is answered with a
   * connection request and the keystroke never reaches the field.
   */
  function handlePaste(event: ClipboardEvent, portId: string): void {
    const text = event.clipboardData?.getData('text/plain') ?? ''
    const reference = parsePortReference(text)
    if (reference === undefined) return
    event.preventDefault()
    onreference?.(reference, portId)
  }
</script>

<section class="controls nodrag">
  {#each definition.inputs as input}
    <div class="row">
      <NodePort {nodeId} port={input} direction="input" {layout} connection={connections.find((item) => item.portId === input.portId && item.direction === 'input')} messages={messages.filter((message) => message.portId === input.portId)} oncontextmenu={onportmenu} />
      {#if isConnected(input.portId)}
        <output title={upstreamText(input.portId)}>{upstreamText(input.portId)}</output>
      {:else}
        <input
          class="expression"
          value={parameterText(stored(input.portId))}
          placeholder={input.valueType}
          aria-label={`${input.label} value`}
          title="Type a value, or paste a copied port reference to wire it up"
          disabled={onparameter === undefined}
          onpaste={(event) => handlePaste(event, input.portId)}
          onblur={(event) => commit(event, input.portId, input.valueType)}
          onkeydown={(event) => { if (event.key === 'Enter') event.currentTarget.blur() }}
        />
      {/if}
      <small>{input.valueType}</small>
    </div>
  {/each}
  {#each definition.parameters.filter((parameter) => !definition.inputs.some((input) => input.portId === parameter.parameterId)) as parameter}
    <div class="row internal">
      <span>{parameter.label}</span>
      <input
        class="expression"
        value={parameterText(stored(parameter.parameterId))}
        placeholder={parameter.valueType}
        aria-label={`${parameter.label} value`}
        disabled={onparameter === undefined}
        onblur={(event) => commit(event, parameter.parameterId, parameter.valueType)}
        onkeydown={(event) => { if (event.key === 'Enter') event.currentTarget.blur() }}
      />
      <small>{parameter.valueType}</small>
    </div>
  {/each}
</section>
<section class="outputs">
  {#each definition.outputs as output}<NodePort {nodeId} port={output} direction="output" {layout} value={outputText(output.portId)} connection={connections.find((item) => item.portId === output.portId && item.direction === 'output')} messages={messages.filter((message) => message.portId === output.portId)} oncontextmenu={onportmenu} />{/each}
</section>

<style>
  /* Zero horizontal padding: the port rows reach the node's edges so the
     handles can sit on them. See NodePort's --port-inset-inline. */
  .controls { padding: 6px 0 5px; }
  .row { display: grid; grid-template-columns: minmax(70px, 1fr) minmax(58px, .8fr) 38px; align-items: center; min-height: 25px; padding-right: 8px; border-bottom: 1px solid color-mix(in srgb, var(--border) 55%, transparent); }
  .row.internal { padding-left: 14px; }
  .row > span { overflow: hidden; color: var(--text-muted); font-size: 9px; text-overflow: ellipsis; white-space: nowrap; }
  output { overflow: hidden; min-height: 19px; padding: 4px 5px; background: var(--canvas); color: var(--text-faint); font: 9px/1.2 ui-monospace, monospace; text-overflow: ellipsis; white-space: nowrap; }
  .expression { width: 100%; min-width: 0; height: 21px; padding: 0 5px; border: 1px solid transparent; border-radius: 2px; background: var(--canvas); color: var(--text); font: 9px/1.2 ui-monospace, monospace; }
  .expression:hover:not(:disabled) { border-color: var(--border); }
  .expression:focus { border-color: var(--node-color); outline: none; }
  .expression::placeholder { color: var(--text-faint); }
  small { padding-left: 5px; overflow: hidden; color: var(--text-faint); font: 7px/1 ui-monospace, monospace; text-overflow: ellipsis; }
  .outputs { display: grid; padding: 3px 0 7px; border-top: 1px solid var(--border); }
</style>
