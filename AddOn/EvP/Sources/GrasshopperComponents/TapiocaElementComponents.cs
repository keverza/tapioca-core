using System;
using System.Collections.Generic;
using System.Drawing;
using System.Text.Json;

using Grasshopper.Kernel;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// What Archicad knows about elements from their header: type, story, angle.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Over <c>Tapioca.GetElementInfo</c>, which exists because these fields
    /// live on the element HEADER and are therefore available for every element
    /// type — its own comment makes the point against Tapir's equivalent, which
    /// returns "NotYetSupportedElementTypeDetails" for a roof.
    /// </para>
    /// <para>
    /// ⚠️ ONE ROW PER INPUT GUID, MISSES INCLUDED. The command promises
    /// positional alignment and says why: "a short list would silently shift
    /// every later element's data onto the wrong guid". So Found is an output
    /// rather than a filter — drop the rows yourself if you want them gone, but
    /// the lists that come out of here can always be zipped against the guids
    /// that went in.
    /// </para>
    /// <para>
    /// Story is <c>floorInd</c>, Archicad's own story index, which is signed:
    /// 0 is the ground story of the project and basements are negative.
    /// </para>
    /// </remarks>
    public class TapiocaElementInfoComponent : GH_Component
    {
        public TapiocaElementInfoComponent()
            : base(
                "Tapioca Element Info",
                "TapiocaInfo",
                "Type, story and rotation of Archicad elements, read from the element header.",
                "Tapioca",
                "Archicad")
        {
        }

        protected override void RegisterInputParams(GH_InputParamManager pManager)
        {
            pManager.AddTextParameter("Elements", "E", "Element GUIDs.", GH_ParamAccess.list);
        }

        protected override void RegisterOutputParams(GH_OutputParamManager pManager)
        {
            pManager.AddBooleanParameter(
                "Found", "F", "Whether each GUID still resolves to an element.", GH_ParamAccess.list);
            pManager.AddTextParameter("Type", "T", "Archicad's element type id.", GH_ParamAccess.list);
            pManager.AddIntegerParameter(
                "Story", "S", "Story index (floorInd); 0 is the ground story, basements are negative.",
                GH_ParamAccess.list);
            pManager.AddNumberParameter(
                "Angle", "A", "Rotation in radians, for the types that carry one (objects and lamps).",
                GH_ParamAccess.list);
        }

        protected override void SolveInstance(IGH_DataAccess DA)
        {
            List<string> guids = new List<string>();
            if (!DA.GetDataList(0, guids) || guids.Count == 0)
            {
                return;
            }

            string response = TapiocaBridge.Call("Tapioca.GetElementInfo", Reply.ElementsRequest(guids));
            if (!Envelope.IsOk(response))
            {
                AddRuntimeMessage(GH_RuntimeMessageLevel.Warning, Envelope.ErrorOf(response));
                return;
            }

            IList<JsonElement> records = Reply.Records(Envelope.DataOf(response), "infoOfElements");
            List<bool> found = new List<bool>();
            List<string> types = new List<string>();
            List<int> stories = new List<int>();
            List<double> angles = new List<double>();

            int hits = 0;
            for (int index = 0; index < records.Count; index++)
            {
                JsonElement record = records[index];
                bool ok = Reply.Flag(record, "found");
                found.Add(ok);
                types.Add(Reply.Text(record, "type"));
                stories.Add(Reply.Whole(record, "floorInd"));
                angles.Add(Reply.Real(record, "angle"));
                if (ok)
                {
                    hits++;
                }
            }

            DA.SetDataList(0, found);
            DA.SetDataList(1, types);
            DA.SetDataList(2, stories);
            DA.SetDataList(3, angles);

            if (hits < records.Count)
            {
                AddRuntimeMessage(
                    GH_RuntimeMessageLevel.Remark, "Resolved " + Reply.Summarise(hits, records.Count) + " elements.");
            }
        }

        protected override Bitmap Icon
        {
            get { return null; }
        }

        public override Guid ComponentGuid
        {
            get { return new Guid("4a9d2c58-71b6-4e03-9d8f-6c25e1a74b90"); }
        }
    }

    /// <summary>
    /// The element ID Archicad shows in its own palettes, per element.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Over <c>Tapioca.GetElementIds</c>. This is the ID a user reads and sorts
    /// by in Archicad — the nearest thing to a native property a definition can
    /// currently pick up, which is why it has a component of its own rather than
    /// living behind hand-written JSON.
    /// </para>
    /// <para>
    /// ⚠️ READ ONLY, AND THE WRITE IS DELIBERATELY ABSENT. Tapioca.SetElementIds
    /// exists and would renumber elements, but it is a WriteCommand and the
    /// Grasshopper bridge refuses those by design ("killing a worker cannot
    /// un-commit an Archicad change", GhBridge.cpp). A component for it would be
    /// a component that always fails.
    /// </para>
    /// <para>
    /// Reason distinguishes the two failures a caller has to tell apart, and the
    /// command distinguishes them for exactly this purpose: "notFound" means the
    /// guid is stale, "noInfoString" means the type has no ID field at all.
    /// </para>
    /// </remarks>
    public class TapiocaElementIdComponent : GH_Component
    {
        public TapiocaElementIdComponent()
            : base(
                "Tapioca Element ID",
                "TapiocaID",
                "The Archicad element ID and type name of each element.",
                "Tapioca",
                "Archicad")
        {
        }

        protected override void RegisterInputParams(GH_InputParamManager pManager)
        {
            pManager.AddTextParameter("Elements", "E", "Element GUIDs.", GH_ParamAccess.list);
        }

        protected override void RegisterOutputParams(GH_OutputParamManager pManager)
        {
            pManager.AddBooleanParameter("Found", "F", "Whether each element has an ID.", GH_ParamAccess.list);
            pManager.AddTextParameter("ID", "I", "The element ID as Archicad shows it.", GH_ParamAccess.list);
            pManager.AddTextParameter("Type", "T", "Element type name.", GH_ParamAccess.list);
            pManager.AddTextParameter(
                "Reason", "R", "Why an ID is missing: notFound (stale GUID) or noInfoString (type has no ID).",
                GH_ParamAccess.list);
        }

        protected override void SolveInstance(IGH_DataAccess DA)
        {
            List<string> guids = new List<string>();
            if (!DA.GetDataList(0, guids) || guids.Count == 0)
            {
                return;
            }

            string response = TapiocaBridge.Call("Tapioca.GetElementIds", Reply.ElementsRequest(guids));
            if (!Envelope.IsOk(response))
            {
                AddRuntimeMessage(GH_RuntimeMessageLevel.Warning, Envelope.ErrorOf(response));
                return;
            }

            IList<JsonElement> records = Reply.Records(Envelope.DataOf(response), "identities");
            List<bool> found = new List<bool>();
            List<string> ids = new List<string>();
            List<string> types = new List<string>();
            List<string> reasons = new List<string>();

            int hits = 0;
            for (int index = 0; index < records.Count; index++)
            {
                JsonElement record = records[index];
                bool ok = Reply.Flag(record, "found");
                found.Add(ok);
                ids.Add(Reply.Text(record, "value"));
                types.Add(Reply.Text(record, "typeName"));
                reasons.Add(Reply.Text(record, "reason"));
                if (ok)
                {
                    hits++;
                }
            }

            DA.SetDataList(0, found);
            DA.SetDataList(1, ids);
            DA.SetDataList(2, types);
            DA.SetDataList(3, reasons);

            if (hits < records.Count)
            {
                AddRuntimeMessage(
                    GH_RuntimeMessageLevel.Remark, "Read " + Reply.Summarise(hits, records.Count) + " element IDs.");
            }
        }

        protected override Bitmap Icon
        {
            get { return null; }
        }

        public override Guid ComponentGuid
        {
            get { return new Guid("f38c6b02-5d41-49ae-b7c2-0e93a5d16f74"); }
        }
    }
}
