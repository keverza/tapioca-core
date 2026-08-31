<script lang="ts">
  import type { EditorTool, EffectiveTool } from './annotations'

  let {
    tool,
    effectiveTool,
    disabled,
    onchange,
  }: {
    tool: EditorTool
    effectiveTool: EffectiveTool
    disabled: boolean
    onchange: (tool: EditorTool) => void
  } = $props()
</script>

<div class="tool-strip" role="toolbar" aria-label="Canvas tools">
  <button
    type="button"
    class:active={tool === 'eraser'}
    aria-pressed={tool === 'eraser'}
    disabled={disabled}
    title="Erase nodes and connections along a pointer path"
    onclick={() => onchange(tool === 'eraser' ? 'select' : 'eraser')}
  ><span aria-hidden="true">&#x232b;</span>Eraser</button>
  <button
    type="button"
    class:active={tool === 'rectangle'}
    aria-pressed={tool === 'rectangle'}
    disabled={disabled}
    title="Draw a presentation-only rectangle"
    onclick={() => onchange(tool === 'rectangle' ? 'select' : 'rectangle')}
  ><span class="rectangle-icon" aria-hidden="true"></span>Rectangle</button>
  <div class:active={effectiveTool === 'knife'} class="knife-indicator" title="Hold Ctrl and drag across connections">
    <span aria-hidden="true">&#x2702;</span>Ctrl
  </div>
</div>
