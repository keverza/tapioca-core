<script lang="ts">
  /**
   * ONE PROMOTED PROPERTY, drawn as a row under the node it was promoted from.
   *
   * ⚠️ THIS IS AN ORDINARY NODE. It is an `archicad.element.setting` with a
   * `parentId`, not a decoration on its host: the wire leaving its nub is a real
   * edge from a real node, so evaluation, typing, undo and the dependency graph
   * all work without knowing this component exists. "Convert to explicit node"
   * clears one layout field and the same node reappears as a box - which is why
   * the conversion cannot lose a downstream link.
   *
   * ⚠️ AND IT DRAWS NO INPUT HANDLE. Its `elements` input is wired to its host by
   * the promotion itself and is the only thing that may ever feed it; offering a
   * target nub would invite a second source for a value whose whole meaning is
   * "this property, of THAT node's elements". Deleting the row is how you undo
   * it, and the row can be dragged out to a box if a different source is wanted.
   */
  import { Handle, Position, type Node, type NodeProps } from '@xyflow/svelte'
  import type { SchemaNodeData } from '../../types'
  import { categoryColor } from '../types/display'

  let { id, data, selected }: NodeProps<Node<SchemaNodeData>> = $props()

  const status = $derived(data.result?.status ?? 'pending')
  const color = $derived(data.visual?.color ?? categoryColor(data.schema.category))

  // The alias the promotion recorded as a nickname, falling back to the setting
  // id and then to the type's own label - never to nothing, because a row with
  // no name is a nub with no explanation.
  const label = $derived(data.visual?.nickname ?? parameterText('setting') ?? data.schema.label)
  const settingId = $derived(parameterText('setting') ?? '')
  const elementType = $derived(parameterText('elementType') ?? '')

  function parameterText(parameterId: string): string | undefined {
    const text = data.parameters.find((parameter) => parameter.parameterId === parameterId)?.value?.text
    return text === undefined || text === '' ? undefined : text
  }

  /**
   * What the row shows on its right, beside the nub.
   *
   * ⚠️ THE TYPE, NOT THE VALUE, WHEN THERE IS A CHOICE. §19: the graph has to
   * stay readable when no Archicad selection exists, and a row that showed only
   * a live number would be blank exactly when someone is trying to understand
   * the graph offline. A count is shown for a list because "12 values" is a
   * shape rather than a reading.
   */
  const summary = $derived.by(() => {
    const output = data.result?.outputs?.find((record) => record.portId === 'values')
    if (output === undefined) return ''
    const count = output.value?.itemCount ?? output.value?.items?.length
    if (count !== undefined) return `${count}`
    return output.value?.text ?? ''
  })
</script>

<article
  class:selected
  class:error={status === 'error'}
  class:inert={status === 'blocked' || status === 'disabled' || status === 'cancelled'}
  style={`--node-color: ${color}`}
  title={elementType === '' ? settingId : `${elementType}.${settingId}`}
>
  <span class="glyph" aria-hidden="true">◇</span>
  <span class="label">{label}</span>
  <span class="summary">{summary}</span>
  <!--
    ⚠️ THE HANDLE ID IS THE PORT'S, NOT THE ROW'S. It is the node's real
    `values` output; naming it after the promotion would make an edge that no
    longer validated against the node's own schema.
  -->
  <Handle type="source" id="values" position={Position.Right} />
</article>

<style>
  article {
    position: relative;
    display: flex;
    width: 100%;
    height: 100%;
    min-height: 18px;
    align-items: center;
    padding: 0 8px 0 6px;
    border: 1px solid var(--border);
    border-left: 3px solid var(--node-color);
    border-radius: 2px;
    background: var(--surface);
    color: var(--text);
    gap: 5px;
  }
  article.selected { border-color: var(--node-color); box-shadow: 0 0 0 1px var(--node-color); }
  article.error { border-color: var(--danger); }
  article.inert { filter: saturate(.55); }
  .glyph { color: var(--text-faint); font-size: 8px; }
  .label { overflow: hidden; flex: 1 1 auto; font: 9px/1 'Segoe UI', sans-serif; text-overflow: ellipsis; white-space: nowrap; }
  .summary { color: var(--text-faint); font: 8px/1 ui-monospace, monospace; }
</style>
