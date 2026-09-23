using System;
using System.Collections.Generic;
using Grasshopper2.Components;
using Grasshopper2.Doc;
using Grasshopper2.UI;
using GrasshopperIO;

namespace TapiocaGH2
{
    [IoId("9efec712-4356-45d9-9777-8aecd2b3e557")]
    public sealed class TapiocaNumberInput : Component, IGuidAware
    {
        private Guid instanceIdBeforeRemap;

        // Distinct from the type's IoId and from the canvas instance ID.
        // The palette uses this stable value even if the display name changes.
        public Guid ExposureId { get; private set; }

        public TapiocaNumberInput()
            : base(new Nomen("Tapioca Number Input", "Solution-scoped number source.", "Tapioca", "Input"))
        {
            ExposureId = Guid.NewGuid();
            instanceIdBeforeRemap = InstanceId;
        }

        public TapiocaNumberInput(IReader reader) : base(reader)
        {
            ExposureId = reader.HasItem((Name) "ExposureId")
                ? reader.Guid128((Name) "ExposureId") : Guid.NewGuid();
            if (ExposureId == Guid.Empty)
                ExposureId = Guid.NewGuid();
            instanceIdBeforeRemap = InstanceId;
        }

        public override void Store(IWriter writer)
        {
            base.Store(writer);
            writer.Guid128((Name) "ExposureId", ExposureId);
        }

        // GH2 remaps canvas instance IDs on paste/merge. The source's exposure
        // must be reminted then; ordinary save/reopen keeps the old value.
        public void ApplyIdMap(Dictionary<Guid, Guid> map)
        {
            if (map.TryGetValue(instanceIdBeforeRemap, out Guid updated) && updated == InstanceId)
            {
                ExposureId = Guid.NewGuid();
                instanceIdBeforeRemap = InstanceId;
            }
        }

        protected override void AddInputs(InputAdder inputs)
        {
            inputs.AddNumber("Authored Default", "D", "Saved value used without a Player override.").Set(1.0);
        }

        protected override void AddOutputs(OutputAdder outputs)
        {
            outputs.AddNumber("Number", "N", "Authored default or transient Player value.");
        }

        protected override void Process(IDataAccess access)
        {
            access.GetItem(0, out double authoredDefault);
            double value = PlayerOverride.TryGet(access.Solution.Document, ExposureId.ToString("D"), out double staged)
                ? staged : authoredDefault;
            access.SetItem(0, value);
        }
    }
}
