<script lang="ts">
  let {
    hasViewer,
    hasReference,
    oninspect,
    onrename,
    onview,
    oncontrols,
    ontoggleenabled,
    onclose,
  }: {
    hasViewer: boolean
    hasReference: boolean
    oninspect: () => void
    onrename: () => void
    onview: () => void
    oncontrols: () => void
    ontoggleenabled: () => void
    onclose: () => void
  } = $props()

  const actions = $derived([
    { label: 'Info', run: oninspect, enabled: true },
    { label: hasViewer ? 'Viewer' : 'View', run: onview, enabled: hasViewer || hasReference },
    { label: 'State', run: ontoggleenabled, enabled: true },
    { label: 'Controls', run: oncontrols, enabled: true },
    { label: 'Rename', run: onrename, enabled: true },
    { label: 'Data', run: oninspect, enabled: true },
  ])
</script>

<div class="radial nodrag" role="menu" aria-label="Quick node actions">
  <div class="ring"></div>
  {#each actions as action, index}
    <button
      type="button"
      role="menuitem"
      disabled={!action.enabled}
      style={`--angle: ${index * 60 - 90}deg`}
      onclick={() => { action.run(); onclose() }}
    >{action.label}</button>
  {/each}
  <button type="button" class="center" onclick={onclose}>Close</button>
</div>

<style>
  .radial { position: absolute; z-index: 15; top: 50%; left: 50%; width: 184px; height: 184px; transform: translate(-50%, -50%); filter: drop-shadow(0 14px 24px rgb(0 0 0 / 48%)); }
  .ring { position: absolute; inset: 8px; border: 1px solid var(--border); border-radius: 50%; background: repeating-conic-gradient(from -30deg, var(--surface-raised) 0deg 58deg, var(--border) 58deg 60deg); mask: radial-gradient(circle, transparent 0 42%, #000 43%); }
  .radial > button:not(.center) { position: absolute; top: 50%; left: 50%; width: 54px; height: 28px; padding: 0; transform: translate(-50%, -50%) rotate(var(--angle)) translateX(60px) rotate(calc(-1 * var(--angle))); border: 0; background: transparent; color: var(--text-muted); font: 8px/1 ui-monospace, monospace; }
  .radial > button:not(.center):hover:not(:disabled) { background: color-mix(in srgb, var(--node-color) 24%, transparent); color: var(--text); }
  .center { position: absolute; top: 50%; left: 50%; width: 58px; height: 58px; padding: 0; transform: translate(-50%, -50%); border: 1px solid var(--node-color); border-radius: 50%; background: var(--surface); color: var(--node-color); font-size: 8px; }
</style>
