using Grasshopper2.Components;
using Grasshopper2.Data;
using Grasshopper2.Parameters;
using Grasshopper2.UI;
using GrasshopperIO;

namespace TapiocaGH2;

// A pure reference operation: deduplicate without resolving headers or geometry.
[IoId("8a6fe2d8-5b6b-4824-ae43-1d5506a81b61")]
public sealed class ArchicadUniqueElements : Component
{
    public ArchicadUniqueElements() : base(new Nomen("Unique Archicad Elements",
        "Keep the first occurrence of each project-bound element reference in each twig.",
        "Tapioca", "Util")) { }
    public ArchicadUniqueElements(IReader reader) : base(reader) { }

    protected override void AddInputs(InputAdder inputs) => inputs.Add(new AcElementParameter(
        "Elements", "E", "Archicad element references; no Archicad read is performed.", Access.Twig));

    protected override void AddOutputs(OutputAdder outputs) => outputs.Add(new AcElementParameter(
        "Unique Elements", "E", "First occurrence of each reference, with original metadata.", Access.Twig));

    protected override void Process(IDataAccess access)
    {
        if (!access.GetPears<AcElementRef>(0, out var pears)) return;
        var seen = new HashSet<AcElementRef>();
        var unique = new List<Pear<AcElementRef>>();
        foreach (var pear in pears)
        {
            if (pear is null || !AcElementData.IsValid(pear.Item))
            {
                access.AddWarning("Invalid reference", "The input contains an invalid Archicad element reference.");
                return;
            }
            if (seen.Add(pear.Item)) unique.Add(pear);
        }
        access.SetTwig(0, Garden.TwigFromPears(unique));
    }
}
