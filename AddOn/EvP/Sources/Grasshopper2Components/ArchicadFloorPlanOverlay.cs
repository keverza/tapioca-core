using Grasshopper2.UI;
using GrasshopperIO;

namespace TapiocaGH2;

[IoId("7a120dfe-5cf1-4a02-9233-771aa947f5c0")]
public sealed class ArchicadFloorPlanOverlay : ArchicadOverlayComponent
{
    protected override bool Plan => true;
    public ArchicadFloorPlanOverlay() : base(new Nomen("Floor Plan Overlay",
        "Show supplied geometry projected onto XY in Archicad's floor plan. Explicit Show/Clear; no storey cut or BIM writes.",
        "Tapioca", "Display")) { }
    public ArchicadFloorPlanOverlay(IReader reader) : base(reader) { }
}
