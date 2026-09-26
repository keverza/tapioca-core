using Grasshopper2.Components;
using Grasshopper2.Parameters;
using Grasshopper2.UI;
using GrasshopperIO;

namespace TapiocaGH2;

// The indices are canvas controls and carry named presets for GH2's Preset
// Picker. The output is the project's actual name, never an attribute index.
public abstract class ArchicadChoiceInput : ArchicadInputBase
{
    protected ArchicadChoiceInput(Nomen nomen) : base(nomen) { }
    protected ArchicadChoiceInput(IReader reader) : base(reader) { }

    protected abstract string[] Choices();

    protected override void AddInputs(InputAdder inputs) =>
        inputs.AddInteger("Choice", "C", "Connect a GH2 Preset Picker here, or use a zero-based position in Available.").Set(0);

    protected override void AddOutputs(OutputAdder outputs)
    {
        outputs.AddText("Selected", "S", "Selected project attribute name.");
        outputs.AddText("Available", "A", "Names from the last successful Archicad project check.", Access.Twig);
    }

    protected override void Process(IDataAccess access)
    {
        access.GetItem(0, out int index);
        string[] options = Choices();
        access.SetTwig(1, options, null, null);
        if (options.Length == 0)
        {
            access.AddWarning("No project choices", "Attach to Archicad, or wait for its project-choice refresh to finish.");
            return;
        }
        if (index < 0 || index >= options.Length)
        {
            access.AddWarning("Choice out of range", $"Choose a position from 0 to {options.Length - 1} (got {index}).");
            return;
        }
        access.SetItem(0, options[index]);
    }
}

[IoId("0c611705-95dc-4391-bc24-89ed73fd942f")]
public sealed class ArchicadLayerInput : ArchicadChoiceInput
{
    public ArchicadLayerInput() : base(new Nomen("Archicad Layer", "Choose an existing project layer.", "Tapioca", "Input")) =>
        ArchicadPresetBindings.Register(this);
    public ArchicadLayerInput(IReader reader) : base(reader) => ArchicadPresetBindings.Register(this);
    protected override string[] Choices() => ArchicadProjectOptions.Read().Layers;
}

[IoId("3cd387a2-d10a-4925-a8a4-3ec5ab5b507a")]
public sealed class ArchicadLineTypeInput : ArchicadChoiceInput
{
    public ArchicadLineTypeInput() : base(new Nomen("Archicad Line Type", "Choose an existing project line type.", "Tapioca", "Input")) =>
        ArchicadPresetBindings.Register(this);
    public ArchicadLineTypeInput(IReader reader) : base(reader) => ArchicadPresetBindings.Register(this);
    protected override string[] Choices() => ArchicadProjectOptions.Read().LineTypes;
}

[IoId("ea30714c-a8c9-4db6-85db-70fc83835fe5")]
public sealed class ArchicadStoryInput : ArchicadInputBase
{
    public ArchicadStoryInput() : base(new Nomen("Archicad Story", "Choose an existing project story.", "Tapioca", "Input")) =>
        ArchicadPresetBindings.Register(this);
    public ArchicadStoryInput(IReader reader) : base(reader) => ArchicadPresetBindings.Register(this);

    protected override void AddInputs(InputAdder inputs) =>
        inputs.AddInteger("Choice", "C", "Connect a GH2 Preset Picker here, or use a zero-based position in Available.").Set(0);

    protected override void AddOutputs(OutputAdder outputs)
    {
        outputs.AddInteger("Story Index", "I", "Selected Archicad story index (not its position in the list).");
        outputs.AddText("Story Name", "N", "Selected story name.");
        outputs.AddNumber("Level", "L", "Selected story elevation in project/world units.");
        outputs.AddText("Available", "A", "Story indices and names from the last successful project check.", Access.Twig);
    }

    protected override void Process(IDataAccess access)
    {
        access.GetItem(0, out int position);
        ArchicadProjectOptions.Story[] stories = ArchicadProjectOptions.Read().Stories;
        access.SetTwig(3, stories.Select(story => $"{story.Index}: {story.Name}").ToArray(), null, null);
        if (stories.Length == 0)
        {
            access.AddWarning("No project stories", "Attach to Archicad, or wait for its project-choice refresh to finish.");
            return;
        }
        if (position < 0 || position >= stories.Length)
        {
            access.AddWarning("Choice out of range", $"Choose a position from 0 to {stories.Length - 1} (got {position}).");
            return;
        }
        ArchicadProjectOptions.Story selected = stories[position];
        access.SetItem(0, selected.Index);
        access.SetItem(1, selected.Name);
        access.SetItem(2, selected.Level);
    }
}
