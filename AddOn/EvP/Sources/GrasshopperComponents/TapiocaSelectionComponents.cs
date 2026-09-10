using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;

using Grasshopper.Kernel;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// Reads Archicad's current selection.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The first half of the gesture this package exists for: pick elements in
    /// Archicad, work on them in Grasshopper. It reports GUIDS ONLY, because that
    /// is all <c>Tapioca.GetSelection</c> returns — everything else about those
    /// elements is a separate, explicit read (Element Info, Element ID), so a
    /// definition that only needs the guids pays for nothing more.
    /// </para>
    /// <para>
    /// ⚠️ IT DOES NOT FOLLOW THE SELECTION, AND THAT IS NOT AN OVERSIGHT. There
    /// is no selection-changed message on the bridge, so a component that
    /// claimed to track the selection would show a stale one and look broken.
    /// Refresh is a toggle the user drives, exactly as Tapioca Status is.
    /// </para>
    /// </remarks>
    public class TapiocaSelectionComponent : GH_Component
    {
        public TapiocaSelectionComponent()
            : base(
                "Tapioca Selection",
                "TapiocaSel",
                "The elements currently selected in Archicad, as GUIDs.",
                "Tapioca",
                "Archicad")
        {
        }

        protected override void RegisterInputParams(GH_InputParamManager pManager)
        {
            pManager.AddBooleanParameter(
                "Refresh",
                "R",
                "Toggle to read the selection again.",
                GH_ParamAccess.item,
                true);
        }

        protected override void RegisterOutputParams(GH_OutputParamManager pManager)
        {
            pManager.AddTextParameter("Elements", "E", "Selected element GUIDs.", GH_ParamAccess.list);
            pManager.AddIntegerParameter("Count", "N", "How many are selected.", GH_ParamAccess.item);
        }

        protected override void SolveInstance(IGH_DataAccess DA)
        {
            bool refresh = true;
            DA.GetData(0, ref refresh);

            string response = TapiocaBridge.Call("Tapioca.GetSelection", string.Empty);
            if (!Envelope.IsOk(response))
            {
                AddRuntimeMessage(GH_RuntimeMessageLevel.Warning, Envelope.ErrorOf(response));
                DA.SetData(1, 0);
                return;
            }

            List<string> guids = new List<string>();
            IList<System.Text.Json.JsonElement> records = Reply.Records(Envelope.DataOf(response), "elements");
            for (int index = 0; index < records.Count; index++)
            {
                string guid = Reply.GuidOf(records[index]);
                if (guid.Length > 0)
                {
                    guids.Add(guid);
                }
            }

            DA.SetDataList(0, guids);
            DA.SetData(1, guids.Count);
        }

        protected override Bitmap Icon
        {
            get { return null; }
        }

        public override Guid ComponentGuid
        {
            get { return new Guid("b2e7c410-6d38-4f52-9a71-c48f0d5b3e26"); }
        }
    }

    /// <summary>
    /// Changes Archicad's selection.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The other half: what a definition worked out, selected in Archicad, so
    /// the user can carry on with Archicad's own tools. "Add to selection like
    /// the Tapioca commands already can", over
    /// <c>Tapioca.ModifySelection</c>.
    /// </para>
    /// <para>
    /// ⚠️ CHANGING A SELECTION IS NOT A MODEL WRITE, WHICH IS WHY THIS IS
    /// ALLOWED AT ALL. The Grasshopper bridge refuses commands that modify the
    /// project — GhBridge.cpp says why: "killing a worker cannot un-commit an
    /// Archicad change". The selection commands are MainThreadCommands rather
    /// than WriteCommands: nothing in the model changes, nothing lands in the
    /// undo stack, and the worst outcome is a selection the user re-picks.
    /// </para>
    /// <para>
    /// ⚠️ GATED ON Run, AND OFF BY DEFAULT. Grasshopper re-solves whenever
    /// anything upstream twitches; a component that selected on every solve
    /// would take the selection away from someone mid-edit. The toggle is the
    /// difference between a definition that reads and one that reaches over.
    /// </para>
    /// <para>
    /// A guid that no longer resolves is reported in Missing rather than
    /// dropped, because selecting the wrong set — or an empty one — quietly is
    /// the failure worth naming.
    /// </para>
    /// </remarks>
    public class TapiocaSelectComponent : GH_Component
    {
        public TapiocaSelectComponent()
            : base(
                "Tapioca Select",
                "TapiocaSelect",
                "Selects elements in Archicad: add to, remove from, replace or clear the current selection.",
                "Tapioca",
                "Archicad")
        {
        }

        protected override void RegisterInputParams(GH_InputParamManager pManager)
        {
            pManager.AddTextParameter(
                "Elements",
                "E",
                "Element GUIDs to select. Not needed when Mode is clear.",
                GH_ParamAccess.list);
            pManager[0].Optional = true;

            pManager.AddTextParameter(
                "Mode",
                "M",
                "add, remove, replace or clear.",
                GH_ParamAccess.item,
                "add");

            pManager.AddBooleanParameter(
                "Run",
                "R",
                "Set true to change Archicad's selection. Left false, this component does nothing.",
                GH_ParamAccess.item,
                false);
        }

        protected override void RegisterOutputParams(GH_OutputParamManager pManager)
        {
            pManager.AddIntegerParameter(
                "Count", "N", "How many elements are selected in Archicad afterwards.", GH_ParamAccess.item);
            pManager.AddTextParameter(
                "Missing", "M", "GUIDs that no longer resolve to an element.", GH_ParamAccess.list);
            pManager.AddTextParameter("Status", "S", "What happened, in words.", GH_ParamAccess.item);
        }

        protected override void SolveInstance(IGH_DataAccess DA)
        {
            List<string> guids = new List<string>();
            string mode = "add";
            bool run = false;
            DA.GetDataList(0, guids);
            DA.GetData(1, ref mode);
            if (!DA.GetData(2, ref run))
            {
                return;
            }

            mode = mode == null ? string.Empty : mode.Trim().ToLowerInvariant();
            if (mode != "add" && mode != "remove" && mode != "replace" && mode != "clear")
            {
                AddRuntimeMessage(
                    GH_RuntimeMessageLevel.Error,
                    "Mode must be add, remove, replace or clear; got \"" + mode + "\".");
                DA.SetData(2, "Not run: unknown mode.");
                return;
            }

            if (!run)
            {
                // A remark rather than a warning: not having pressed Run is the
                // normal resting state of this component, not a problem with it.
                AddRuntimeMessage(GH_RuntimeMessageLevel.Remark, "Set Run to true to change Archicad's selection.");
                DA.SetData(2, "Not run.");
                return;
            }

            if (mode != "clear" && guids.Count == 0)
            {
                AddRuntimeMessage(GH_RuntimeMessageLevel.Warning, "No element GUIDs to " + mode + ".");
                DA.SetData(2, "Not run: nothing to " + mode + ".");
                return;
            }

            string parameters = "{\"op\":\"" + mode + "\"";
            if (mode != "clear")
            {
                string elements = Reply.ElementsRequest(guids);
                // Splice the elements array into the same object as op, rather
                // than building the whole thing twice: ElementsRequest is the one
                // place the element-id shape is written.
                parameters += "," + elements.Substring(1, elements.Length - 2);
            }

            parameters += "}";

            string response = TapiocaBridge.Call("Tapioca.ModifySelection", parameters);
            if (!Envelope.IsOk(response))
            {
                AddRuntimeMessage(GH_RuntimeMessageLevel.Warning, Envelope.ErrorOf(response));
                DA.SetData(2, Envelope.ErrorOf(response));
                return;
            }

            string data = Envelope.DataOf(response);
            List<string> missing = new List<string>();
            IList<System.Text.Json.JsonElement> misses = Reply.Records(data, "missing");
            for (int index = 0; index < misses.Count; index++)
            {
                string guid = Reply.GuidOf(misses[index]);
                if (guid.Length > 0)
                {
                    missing.Add(guid);
                }
            }

            int count = Reply.Count(data, "count");
            int changed = Reply.Count(data, "changed");

            DA.SetData(0, count);
            DA.SetDataList(1, missing);
            DA.SetData(
                2,
                mode + ": " + changed.ToString(CultureInfo.InvariantCulture) + " changed, "
                + count.ToString(CultureInfo.InvariantCulture) + " now selected"
                + (missing.Count == 0
                       ? "."
                       : ", " + missing.Count.ToString(CultureInfo.InvariantCulture) + " missing."));

            if (missing.Count > 0)
            {
                AddRuntimeMessage(
                    GH_RuntimeMessageLevel.Warning,
                    missing.Count.ToString(CultureInfo.InvariantCulture)
                    + " GUID(s) no longer resolve to an element in this project.");
            }
        }

        protected override Bitmap Icon
        {
            get { return null; }
        }

        public override Guid ComponentGuid
        {
            get { return new Guid("d51a8f36-9c24-4e7b-8f13-2a6b7d90c845"); }
        }
    }
}
