using Grasshopper2.Components;
using Grasshopper2.Doc;
using Grasshopper2.UI;
using GrasshopperIO;

namespace TapiocaGH2;

// Project selectors are Player exposures, not just editor widgets. This ID is
// independent of the component type IoId, mutable label and canvas instance.
public abstract class ArchicadInputBase : Component, IGuidAware
{
    private Guid instanceIdBeforeRemap;
    public Guid ExposureId { get; private set; }

    protected ArchicadInputBase(Nomen nomen) : base(nomen)
    {
        ExposureId = Guid.NewGuid();
        instanceIdBeforeRemap = InstanceId;
    }

    protected ArchicadInputBase(IReader reader) : base(reader)
    {
        ExposureId = reader.HasItem((Name)"ExposureId")
            ? reader.Guid128((Name)"ExposureId") : Guid.NewGuid();
        if (ExposureId == Guid.Empty)
            ExposureId = Guid.NewGuid();
        instanceIdBeforeRemap = InstanceId;
    }

    public override void Store(IWriter writer)
    {
        base.Store(writer);
        writer.Guid128((Name)"ExposureId", ExposureId);
    }

    public void ApplyIdMap(Dictionary<Guid, Guid> map)
    {
        if (map.TryGetValue(instanceIdBeforeRemap, out Guid updated) && updated == InstanceId)
        {
            ExposureId = Guid.NewGuid();
            instanceIdBeforeRemap = InstanceId;
        }
    }
}
