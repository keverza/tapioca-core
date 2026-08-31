<script lang="ts">
  import { ViewportPortal } from '@xyflow/svelte'
  import type { EditorAnnotation } from './annotations'

  let {
    annotations,
    draft,
  }: { annotations: EditorAnnotation[]; draft?: EditorAnnotation } = $props()
</script>

<ViewportPortal target="back">
  <div class="annotation-layer" aria-hidden="true">
    {#each annotations as annotation (annotation.id)}
      <div
        class:frame={annotation.kind === 'frame'}
        class:rectangle={annotation.kind === 'rectangle'}
        class="graph-annotation"
        style={`left:${annotation.bounds.x}px;top:${annotation.bounds.y}px;width:${annotation.bounds.width}px;height:${annotation.bounds.height}px`}
      >
        <span>{annotation.label}</span>
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
