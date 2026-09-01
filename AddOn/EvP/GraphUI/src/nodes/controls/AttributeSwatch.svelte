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
   * ⚠️ EVERY <svg> CARRIES EXPLICIT width AND height ATTRIBUTES. An SVG with
   * neither falls back to its replaced-element default of 300x150 CSS pixels the
   * instant the stylesheet does not reach it, which is not a small mistake: it
   * paints a grey slab across the popover, and that is exactly what a "the
   * picker looks broken" report is made of. The CSS sizes it too; the attributes
   * are what make the failure impossible rather than unlikely.
   *
   * Nothing is drawn for an attribute with NO preview - a profile and a layer
   * have none. An empty placeholder box in every row of those two pickers reads
   * as a picture that failed to load, which is worse than a list with no
   * pictures in it. The caller asks `hasSwatch` and reserves no column.
   */
  import type { AttributePreview } from './widgets'
  import { dashArrayFor, patternCells, patternExtent, skinBands } from './widgets'

  /**
   * `part` picks WHICH half of a two-part attribute to draw. A surface is a
   * colour and a hatch, and Archicad's own list shows them in separate columns -
   * name in the middle, hatch to the right - so the row asks for each in turn
   * rather than this component inventing a combined glyph nobody uses elsewhere.
   */
  let {
    preview,
    size = 14,
    part = 'primary',
  }: { preview?: AttributePreview; size?: number; part?: 'primary' | 'pattern' } = $props()

  const extent = patternExtent()
</script>

{#if preview === undefined}
  <!-- Deliberately nothing. See the note above. -->
{:else if preview.kind === 'surface'}
  {#if part === 'pattern'}
    {#if preview.pattern !== undefined}
      <svg class="swatch pattern" width={size} height={size} style={`--size: ${size}px`} viewBox={`0 0 ${extent} ${extent}`} aria-hidden="true">
        {#each patternCells(preview.pattern) as cell (`${cell.x}-${cell.y}`)}
          <rect x={cell.x} y={cell.y} width="1" height="1" />
        {/each}
      </svg>
    {/if}
  {:else}
    <span class="swatch" style={`--size: ${size}px; background: ${preview.color ?? 'transparent'}`}></span>
  {/if}
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
    <svg class="swatch pattern" width={size} height={size} style={`--size: ${size}px`} viewBox={`0 0 ${extent} ${extent}`} aria-hidden="true">
      {#each patternCells(preview.pattern ?? []) as cell (`${cell.x}-${cell.y}`)}
        <rect x={cell.x} y={cell.y} width="1" height="1" />
      {/each}
    </svg>
  {/if}
{:else if preview.kind === 'line'}
  <svg class="swatch line" width={size * 1.7} height={size} style={`--size: ${size}px; --width: ${size * 1.7}px`} viewBox="0 0 24 8" preserveAspectRatio="none" aria-hidden="true">
    <line x1="0" y1="4" x2="24" y2="4" stroke-dasharray={dashArrayFor(preview)} />
  </svg>
{:else}
  <!-- A composite reads as its section: skins stacked in proportion, which is
       what tells a 100 block cavity from a brick single at swatch size. -->
  <svg class="swatch" width={size} height={size} style={`--size: ${size}px`} viewBox="0 0 8 8" preserveAspectRatio="none" aria-hidden="true">
    {#each skinBands(preview.skins ?? []) as band, index (index)}
      <rect x="0" y={band.offset * 8} width="8" height={band.fraction * 8} fill={band.color ?? 'currentColor'} />
    {/each}
  </svg>
{/if}

<style>
  .swatch { display: block; box-sizing: border-box; width: var(--size); height: var(--size); flex: none; overflow: hidden; border: 1px solid var(--border); border-radius: 1px; background: var(--surface); color: var(--text-muted); }
  .swatch.solid { background: currentColor; }
  svg.swatch { shape-rendering: crispEdges; }
  /* Only the bit pattern inks itself from the text colour; a composite's bands
     carry their own fill attribute, and a CSS rule would win over it. */
  .pattern rect { fill: currentColor; }
  /* A line wants length to be legible, so it is the one swatch that is wider
     than it is tall. */
  .line { width: var(--width); border: 0; border-radius: 0; background: transparent; }
  .line line { stroke: currentColor; stroke-width: 1; vector-effect: non-scaling-stroke; }
</style>
