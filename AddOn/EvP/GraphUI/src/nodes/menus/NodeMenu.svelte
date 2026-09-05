<script lang="ts">
  import { closeOnOutsidePress } from '../../interaction'
  import type { ExecutionMode } from '../types/runtime'

  let {
    mode,
    nodeWidth,
    nodeHeight,
    hasViewer,
    hasReference,
    hasBrowser,
    viewerVisible,
    canBypass,
    canHold,
    oninspect,
    onrename,
    onview,
    oncontrols,
    onbrowse,
    ontoggleenabled,
    onbypass,
    onhold,
    onclose,
  }: {
    mode: ExecutionMode
    nodeWidth: number
    nodeHeight: number
    hasViewer: boolean
    hasReference: boolean
    hasBrowser: boolean
    viewerVisible: boolean
    canBypass: boolean
    canHold: boolean
    oninspect: () => void
    onrename: () => void
    onview: () => void
    oncontrols: () => void
    onbrowse: () => void
    ontoggleenabled: () => void
    onbypass: () => void
    onhold: () => void
    onclose: () => void
  } = $props()

  /**
   * The hole is the NODE.
   *
   * So the inner radius is the node's own circumscribed circle - half its
   * diagonal - and not a fixed number: a circular hole only clears a rectangle
   * once it reaches the corners, and anything smaller puts a wedge over the
   * thing the menu is about. The ring grows with the node instead of the node
   * hiding under the ring.
   */
  const BAND = 66
  const CLEARANCE = 12
  const inner = $derived(Math.max(80, Math.hypot(nodeWidth, nodeHeight) / 2 + CLEARANCE))
  const outer = $derived(inner + BAND)
  const size = $derived(Math.ceil(outer * 2) + 4)
  const centre = $derived(size / 2)
  const labelRadius = $derived(inner + BAND / 2)
  // A hairline of empty angle between wedges, so the ring reads as segments
  // rather than as one disc with text on it.
  const GAP_DEGREES = 1.4

  /**
   * Eight segments. The ones that TOGGLE something carry their own colour and
   * fill when they are on, so what is currently true about the node reads from
   * the ring without hovering anything. The rest are plain verbs.
   */
  const actions = $derived([
    { label: 'Info', tone: 'neutral', flag: false, enabled: true, why: '', run: oninspect },
    { label: 'Rename', tone: 'neutral', flag: false, enabled: true, why: '', run: onrename },
    { label: hasViewer ? 'Viewer' : 'View', tone: 'view', flag: hasViewer && viewerVisible, enabled: hasViewer || hasReference, why: 'This node type draws no preview and holds no reference.', run: onview },
    { label: 'Browse', tone: 'browse', flag: false, enabled: hasBrowser, why: 'This node holds no data to browse yet. Evaluate it first.', run: onbrowse },
    { label: mode === 'disabled' ? 'Disabled' : 'Enabled', tone: 'state', flag: mode !== 'disabled', enabled: true, why: '', run: ontoggleenabled },
    { label: 'Bypass', tone: 'bypass', flag: mode === 'bypassed', enabled: canBypass, why: 'This node type declares no bypass mapping.', run: onbypass },
    { label: 'Hold', tone: 'hold', flag: mode === 'holding', enabled: canHold, why: 'This node type is not hold-capable.', run: onhold },
    { label: 'Controls', tone: 'neutral', flag: false, enabled: true, why: '', run: oncontrols },
  ])

  const step = $derived(360 / actions.length)

  function point(angleDegrees: number, radius: number): [number, number] {
    // -90 so segment zero starts at the top, which is where a person reads first.
    const radians = ((angleDegrees - 90) * Math.PI) / 180
    return [centre + radius * Math.cos(radians), centre + radius * Math.sin(radians)]
  }

  /** One annulus wedge, as a closed path: outer arc, in, inner arc back, close. */
  function wedge(index: number): string {
    const from = index * step + GAP_DEGREES / 2
    const to = (index + 1) * step - GAP_DEGREES / 2
    const large = to - from > 180 ? 1 : 0
    const [ox1, oy1] = point(from, outer)
    const [ox2, oy2] = point(to, outer)
    const [ix2, iy2] = point(to, inner)
    const [ix1, iy1] = point(from, inner)
    return [
      `M ${ox1} ${oy1}`,
      `A ${outer} ${outer} 0 ${large} 1 ${ox2} ${oy2}`,
      `L ${ix2} ${iy2}`,
      `A ${inner} ${inner} 0 ${large} 0 ${ix1} ${iy1}`,
      'Z',
    ].join(' ')
  }

  function escape(event: KeyboardEvent): void {
    if (event.key !== 'Escape') return
    event.stopPropagation()
    onclose()
  }

  function activate(action: (typeof actions)[number]): void {
    if (!action.enabled) return
    action.run()
    onclose()
  }
</script>

<!--
  Nothing is drawn inside the inner circle and the container takes no pointer
  events of its own, so the node shows through the hole and stays clickable -
  and a press on it counts as a press outside the menu, which puts the menu
  away. That is the close gesture: click the node the ring is around.
-->
<svelte:window onkeydown={escape} />

<div class="radial nodrag nowheel" role="menu" aria-label="Quick node actions" use:closeOnOutsidePress={onclose}>
  <svg
    viewBox={`0 0 ${size} ${size}`}
    width={size}
    height={size}
    style={`margin-left:${-size / 2}px; margin-top:${-size / 2}px`}
  >
    <circle class="guide" cx={centre} cy={centre} r={inner} />
    {#each actions as action, index}
      {@const [lx, ly] = point(index * step + step / 2, labelRadius)}
      <g
        class={`segment ${action.tone}`}
        class:on={action.flag}
        class:disabled={!action.enabled}
        role="menuitemcheckbox"
        aria-checked={action.flag}
        aria-label={action.label}
        aria-disabled={!action.enabled}
        tabindex={action.enabled ? 0 : -1}
        onclick={() => activate(action)}
        onkeydown={(event) => { if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); activate(action) } }}
      >
        <title>{action.enabled ? action.label : `${action.label} - ${action.why}`}</title>
        <path d={wedge(index)} />
        <text x={lx} y={ly}>{action.label}</text>
      </g>
    {/each}
  </svg>
</div>

<style>
  /* Centred on the node and inert: only the wedges take presses, so the hole is
     the live node rather than a hole in a sheet laid over it. */
  .radial { position: absolute; z-index: 15; top: 50%; left: 50%; width: 0; height: 0; pointer-events: none; }
  /* The shadow lives on the svg, not on the zero-sized container it hangs off. */
  svg { position: absolute; overflow: visible; filter: drop-shadow(0 16px 30px rgb(0 0 0 / 52%)); pointer-events: none; }

  .guide { fill: none; stroke: color-mix(in srgb, var(--node-color) 45%, transparent); stroke-dasharray: 3 4; stroke-width: 1; }

  .segment { cursor: pointer; pointer-events: auto; }
  .segment path { fill: var(--surface-raised); stroke: var(--border); stroke-width: 1; }
  .segment text { fill: var(--text-muted); font: 9px/1 ui-monospace, monospace; pointer-events: none; text-anchor: middle; dominant-baseline: middle; }
  .segment:hover path { fill: color-mix(in srgb, var(--flag) 24%, var(--surface-raised)); stroke: var(--flag); }
  .segment:hover text { fill: var(--text); }
  .segment:focus-visible path { stroke: var(--flag); stroke-width: 2; }
  .segment:focus { outline: none; }
  .segment.disabled { cursor: not-allowed; pointer-events: none; }
  .segment.disabled path { opacity: .4; }
  .segment.disabled text { fill: var(--text-faint); opacity: .55; }
  /* On = the flag's colour fills the wedge; off = the colour is only its edge. */
  .segment.on path { fill: color-mix(in srgb, var(--flag) 38%, var(--surface)); stroke: var(--flag); }
  .segment.on text { fill: var(--text); }
  .segment.view { --flag: #65a9b8; }
  .segment.browse { --flag: #6e91c9; }
  .segment.state { --flag: #7da76a; }
  .segment.bypass { --flag: #a580bd; }
  .segment.hold { --flag: var(--warning); }
  .segment.neutral { --flag: var(--node-color); }
</style>
