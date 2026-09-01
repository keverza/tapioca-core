<script lang="ts">
  /**
   * One attribute's swatch, drawn from the DEFINITION the runtime sent.
   *
   * ⚠️ THE SWATCH IS THE HALF THAT MAKES THE LIST USABLE. "25 %", "50 %" and
   * "75 %" are three indistinguishable words; as patterns they are obvious at a
   * glance. Archicad's own preview is a vector image the API does not hand out,
   * so the runtime sends what the attribute IS - eight bytes of bit pattern, the
   * dash lengths, the skin thicknesses and their colours - and this draws it.
   * The consequence worth keeping: a whole fill list is one small response, and
   * the swatch can be drawn at any size without asking for a bigger picture.
   *
   * SVG rather than canvas: it scales, it survives a DPI change with no redraw,
   * and a hundred rows cost a hundred small trees rather than a hundred
   * contexts.
   */
  import type { AttributePreview } from './widgets'
  import { dashArrayFor, patternCells, skinBands } from './widgets'

  let { preview, size = 14 }: { preview?: AttributePreview; size?: number } = $props()
</script>

{#if preview === undefined}
  <span class="swatch empty" style={`--size: ${size}px`}></span>
{:else if preview.kind === 'color'}
  <span class="swatch" style={`--size: ${size}px; background: ${preview.color ?? 'transparent'}`}></span>
{:else if preview.kind === 'pattern'}
  {#if preview.fillKind === 'empty'}
    <span class="swatch" style={`--size: ${size}px`}></span>
  {:else if preview.fillKind === 'solid'}
    <span class="swatch solid" style={`--size: ${size}px`}></span>
  {:else}
    <!-- The 8x8 bit pattern, one rect per set bit. `shape-rendering: crispEdges`
         keeps a 25 % dot pattern from blurring into flat grey at this size,
         which would defeat the whole point of drawing it. -->
    <svg class="swatch pattern" style={`--size: ${size}px`} viewBox="0 0 8 8" aria-hidden="true">
      {#each patternCells(preview.pattern ?? []) as cell (`${cell.x}-${cell.y}`)}
        <rect x={cell.x} y={cell.y} width="1" height="1" />
      {/each}
    </svg>
  {/if}
{:else if preview.kind === 'line'}
  <svg class="swatch line" style={`--size: ${size}px`} viewBox="0 0 24 8" preserveAspectRatio="none" aria-hidden="true">
    <line x1="0" y1="4" x2="24" y2="4" stroke-dasharray={dashArrayFor(preview)} />
  </svg>
{:else}
  <!-- A composite reads as its section: skins stacked in proportion, which is
       what tells a 100 block cavity from a brick single at swatch size. -->
  <svg class="swatch" style={`--size: ${size}px`} viewBox="0 0 8 8" preserveAspectRatio="none" aria-hidden="true">
    {#each skinBands(preview.skins ?? []) as band, index (index)}
      <rect x="0" y={band.offset * 8} width="8" height={band.fraction * 8} fill={band.color ?? 'currentColor'} />
    {/each}
  </svg>
{/if}

<style>
  .swatch { display: block; width: var(--size); height: var(--size); flex: none; box-sizing: border-box; border: 1px solid var(--border); border-radius: 1px; background: var(--surface); color: var(--text-muted); overflow: hidden; }
  .swatch.empty { border-style: dashed; }
  .swatch.solid { background: currentColor; }
  svg.swatch { shape-rendering: crispEdges; }
  /* Only the bit pattern inks itself from the text colour; a composite's bands
     carry their own fill attribute, and a CSS rule would win over it. */
  .pattern rect { fill: currentColor; }
  .line { border: 0; border-radius: 0; background: transparent; }
  .line line { stroke: currentColor; stroke-width: 1; vector-effect: non-scaling-stroke; }
</style>
