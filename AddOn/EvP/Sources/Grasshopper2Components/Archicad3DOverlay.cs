using Grasshopper2.UI;
using GrasshopperIO;

namespace TapiocaGH2;

[IoId("94af991e-bccb-44c4-8495-061d0673fbc9")]
public sealed class Archicad3DOverlay : ArchicadOverlayComponent
{
    protected override bool Plan => false;
    public Archicad3DOverlay() : base(new Nomen("3D Overlay",
        "Show curves, meshes and points in Archicad's 3D overlay with host occlusion. Explicit Show/Clear; no BIM writes.",
        "Tapioca", "Display")) { }
    public Archicad3DOverlay(IReader reader) : base(reader) { }
}
