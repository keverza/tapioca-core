using Eto.Drawing;
using Grasshopper2.Components;
using Grasshopper2.UI;
using GrasshopperIO;
using GhColour = Grasshopper2.Types.Colour.Colour;

namespace TapiocaGH2;

// A GH2 style value, not an Archicad overlay. The viewport renderer still
// needs its own revision-scoped preview transport before it can consume this.
[IoId("35be9ebe-1510-4fe1-a922-fd6e8e863210")]
public sealed class ArchicadPreviewTint : Component
{
    public ArchicadPreviewTint() : base(new Nomen("Preview Tint",
        "Set a colour and opacity for GH2 preview styling; does not draw in Archicad yet.",
        "Tapioca", "Display")) { }
    public ArchicadPreviewTint(IReader reader) : base(reader) { }

    protected override void AddInputs(InputAdder inputs)
    {
        inputs.AddColour("Colour", "C", "Preview colour.").Set(Colors.CornflowerBlue);
        inputs.AddNumber("Opacity", "O", "Alpha from 0 (transparent) to 1 (opaque).").Set(0.5);
    }

    protected override void AddOutputs(OutputAdder outputs) =>
        outputs.AddColour("Tint", "T", "Colour with alpha for GH2 preview components.");

    protected override void Process(IDataAccess access)
    {
        if (!access.GetItem(0, out GhColour colour) || !access.GetItem(1, out double opacity)) return;
        if (!double.IsFinite(opacity) || opacity is < 0 or > 1)
        {
            access.AddWarning("Invalid opacity", "Opacity must be between zero and one.");
            return;
        }
        access.SetItem(0, colour.WithAlpha((float)opacity));
    }
}
