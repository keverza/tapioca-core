<script lang="ts">
  import { ViewportPortal } from '@xyflow/svelte'
  import type { EditorAnnotation } from './annotations'

  let {
    annotations,
    draft,
    selectedId,
  }: { annotations: EditorAnnotation[]; draft?: EditorAnnotation; selectedId?: string } = $props()
</script>

<ViewportPortal target="back">
  <div class="annotation-layer" aria-hidden="true">
    {#each annotations as annotation (annotation.id)}
      <div
        class:frame={annotation.kind === 'frame'}
        class:rectangle={annotation.kind === 'rectangle'}
        class:selected={annotation.id === selectedId}
        class="graph-annotation"
        style={`left:${annotation.bounds.x}px;top:${annotation.bounds.y}px;width:${annotation.bounds.width}px;height:${annotation.bounds.height}px`}
      >
        {#if annotation.label !== ''}<span>{annotation.label}</span>{/if}
      </div>
    {/each}
    {#if draft !== undefined}
      <div
        class="graph-annotation rectangle draft"
        style={`left:${draft.bounds.x}px;top:${draft.bounds.y}px;width:${draft.bounds.width}px;height:${draft.bounds.height}px`}
      ></div>
    {/if}
  </div>
</ViewportPortal>
