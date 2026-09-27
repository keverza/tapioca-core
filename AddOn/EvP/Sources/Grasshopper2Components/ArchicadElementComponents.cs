using Grasshopper2.Components;
using Grasshopper2.Parameters;
using Grasshopper2.Parameters.Standard;
using Grasshopper2.UI;
using GrasshopperIO;

namespace TapiocaGH2;

// A GUID becomes a BIM reference only when deliberately bound to the active
// project. No Archicad read occurs in a component solution.
[IoId("5cb5887b-df3e-47e5-bc21-eb922d2d16ca")]
public sealed class ArchicadElement : Component
{
    public ArchicadElement() : base(new Nomen("AC Element", "Bind a GUID to the connected Archicad project.",
        "Tapioca", "Archicad")) { ArchicadPresetBindings.Register(this); }
    public ArchicadElement(IReader reader) : base(reader) { ArchicadPresetBindings.Register(this); }

    protected override void AddInputs(InputAdder inputs) => inputs.Add(new GuidParameter(
        "GUID", "G", "Element GUID in the currently connected Archicad project.", Access.Item));

    protected override void AddOutputs(OutputAdder outputs) => outputs.Add(new AcElementParameter(
        "Element", "E", "Project-bound element reference.", Access.Item));

    protected override void Process(IDataAccess access)
    {
        if (!access.GetItem(0, out Guid id) || id == Guid.Empty)
            return;
        string? project = ArchicadProjectOptions.CurrentIdentity();
        if (project is null)
        {
            access.AddWarning("No project", "Attach to Archicad and refresh its project snapshot before binding a GUID.");
            return;
        }
        var element = new AcElementRef(project, id);
        access.SetItem(0, element, AcElementData.Provenance(element));
    }
}
