using System;
using System.Collections.Generic;
using System.Drawing;
using System.Text.Json;

using Grasshopper.Kernel;
using Grasshopper.Kernel.Data;
using Grasshopper.Kernel.Types;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// Everything Archicad knows about an element, in one read.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ ONE COMPONENT, BECAUSE IT WAS ALWAYS ONE CALL. Element Info, Element ID
    /// and Element Properties each sent a request and each pulled a different
    /// slice out of the same records; three components meant three round trips to
    /// read one element, and a canvas with three of them wired to the same guids.
    /// <c>Tapioca.GetElementProperties</c> answers all of it at once — identity,
    /// storey, layer, classifications and properties — so this asks once.
    /// </para>
    /// <para>
    /// ⚠️ STATUS IS AN OUTPUT, AND IT IS THE FIX FOR "DEFINED IN ARCHICAD, BLANK
    /// HERE". A property's value is only meaningful when Archicad says it
    /// evaluated: API_Property carries a status, and on <c>notAvailable</c> (the
    /// property does not apply to this element) or <c>notEvaluated</c> (an
    /// expression that will not resolve, a built-in that cannot be worked out for
    /// this element) the API documents the value field as UNDEFINED. The add-on
    /// used to format it anyway, which produced strings that looked like answers.
    /// Now the value is empty in those cases and Status says which of the three
    /// happened — a blank you can act on instead of a blank you have to guess at.
    /// </para>
    /// <para>
    /// ⚠️ AND THE READ COVERS BUILT-IN PROPERTIES NOW. The old path filtered to
    /// user-defined only, which omitted most of what the Property Manager shows.
    /// Scope selects; the default is everything.
    /// </para>
    /// <para>
    /// ⚠️ TREES FOR THE MANY, LISTS FOR THE ONE. Elements do not share a property
    /// set — a wall may carry six and a zone twenty — so Property, Value, Status,
    /// Group and Classification come out as matched trees, one branch per input
    /// element in input order, EMPTY BRANCHES INCLUDED so that branch index
    /// always equals element index. The per-element facts are plain lists over
    /// the same indices.
    /// </para>
    /// </remarks>
    public class TapiocaElementComponent : GH_Component
    {
        public TapiocaElementComponent()
            : base(
                "Tapioca Element",
                "Tapioca Element",
                "Identity, storey, layer, classifications and properties of Archicad elements, in one read.",
                "Tapioca",
                "Archicad")
        {
        }

        protected override void RegisterInputParams(GH_InputParamManager pManager)
        {
            pManager.AddTextParameter("Elements", "Elements", "Element GUIDs.", GH_ParamAccess.list);
            pManager.AddTextParameter(
                "Scope",
                "Scope",
                "Which properties to read: all (default), user, or builtin.",
                GH_ParamAccess.item,
                "all");
            pManager[1].Optional = true;
        }

        protected override void RegisterOutputParams(GH_OutputParamManager pManager)
        {
            pManager.AddBooleanParameter(
                "Found", "Found", "Whether each GUID still resolves to an element.", GH_ParamAccess.list);
            pManager.AddTextParameter("Type", "Type", "Element type name.", GH_ParamAccess.list);
            pManager.AddTextParameter("ID", "ID", "The element ID Archicad shows in its palettes.", GH_ParamAccess.list);
            pManager.AddTextParameter("Layer", "Layer", "Layer name.", GH_ParamAccess.list);
            pManager.AddTextParameter("Story", "Story", "Home story name.", GH_ParamAccess.list);
            pManager.AddIntegerParameter(
                "Level", "Level", "Story index; 0 is the ground story and basements are negative.",
                GH_ParamAccess.list);
            pManager.AddTextParameter(
                "Classification", "Classification", "Classification item names, one branch per element.",
                GH_ParamAccess.tree);
            pManager.AddTextParameter(
                "Property", "Property", "Property names, one branch per element.", GH_ParamAccess.tree);
            pManager.AddTextParameter(
                "Value", "Value", "Property values, matching Property. Empty unless Status is hasValue.",
                GH_ParamAccess.tree);
            pManager.AddTextParameter(
                "Status",
                "Status",
                "Per property: hasValue, notAvailable (not applicable to this element) or notEvaluated "
                + "(Archicad could not work it out).",
                GH_ParamAccess.tree);
            pManager.AddTextParameter(
                "Group", "Group", "Property group names, matching Property.", GH_ParamAccess.tree);
        }

        protected override void SolveInstance(IGH_DataAccess DA)
        {
            List<string> guids = new List<string>();
            string scope = "all";
            if (!DA.GetDataList(0, guids) || guids.Count == 0)
            {
                return;
            }

            DA.GetData(1, ref scope);
            scope = scope == null ? "all" : scope.Trim().ToLowerInvariant();
            if (scope != "all" && scope != "user" && scope != "builtin")
            {
                AddRuntimeMessage(
                    GH_RuntimeMessageLevel.Error, "Scope must be all, user or builtin; got \"" + scope + "\".");
                return;
            }

            string elements = Reply.ElementsRequest(guids);
            string parameters = "{\"scope\":\"" + scope + "\"," + elements.Substring(1, elements.Length - 2) + "}";

            string response = TapiocaBridge.Call("Tapioca.GetElementProperties", parameters);
            if (!Envelope.IsOk(response))
            {
                AddRuntimeMessage(GH_RuntimeMessageLevel.Warning, Envelope.ErrorOf(response));
                return;
            }

            IList<JsonElement> records = Reply.Records(Envelope.DataOf(response), "propertiesOfElements");

            List<bool> found = new List<bool>();
            List<string> types = new List<string>();
            List<string> ids = new List<string>();
            List<string> layers = new List<string>();
            List<string> stories = new List<string>();
            List<int> levels = new List<int>();
            GH_Structure<GH_String> classifications = new GH_Structure<GH_String>();
            GH_Structure<GH_String> names = new GH_Structure<GH_String>();
            GH_Structure<GH_String> values = new GH_Structure<GH_String>();
            GH_Structure<GH_String> statuses = new GH_Structure<GH_String>();
            GH_Structure<GH_String> groups = new GH_Structure<GH_String>();

            int hits = 0;
            int unevaluated = 0;

            for (int index = 0; index < records.Count; index++)
            {
                JsonElement record = records[index];
                bool ok = Reply.Flag(record, "found");
                found.Add(ok);
                types.Add(Reply.Text(record, "typeName"));
                ids.Add(Reply.Text(record, "elemId"));
                layers.Add(Reply.Text(record, "layer"));
                stories.Add(Reply.Text(record, "story"));
                levels.Add(Reply.Whole(record, "storyIndex"));
                if (ok)
                {
                    hits++;
                }

                GH_Path path = new GH_Path(index);

                // ⚠️ ENSURED EVEN WHEN EMPTY, or an element with no properties
                // would have no branch and every later element's branch index
                // would be one short of its position in the input.
                classifications.EnsurePath(path);
                names.EnsurePath(path);
                values.EnsurePath(path);
                statuses.EnsurePath(path);
                groups.EnsurePath(path);

                // ⚠️ THE CLASSIFICATION NAME, OR THE SYSTEM THAT HAS NONE. The
                // add-on emits one entry per classification SYSTEM the element
                // participates in, including the ones it is unclassified in --
                // dropping those was why a list could come back shorter than the
                // project's systems with nothing saying which was blank.
                JsonElement classArray;
                if (record.ValueKind == JsonValueKind.Object
                    && record.TryGetProperty("classifications", out classArray)
                    && classArray.ValueKind == JsonValueKind.Array)
                {
                    foreach (JsonElement item in classArray.EnumerateArray())
                    {
                        string label = Reply.Flag(item, "found")
                            ? Reply.Text(item, "name")
                            : "(unclassified: " + Reply.Text(item, "system") + ")";
                        classifications.Append(new GH_String(label), path);
                    }
                }

                JsonElement propertyArray;
                if (record.ValueKind == JsonValueKind.Object
                    && record.TryGetProperty("properties", out propertyArray)
                    && propertyArray.ValueKind == JsonValueKind.Array)
                {
                    foreach (JsonElement item in propertyArray.EnumerateArray())
                    {
                        string status = Reply.Text(item, "status");
                        names.Append(new GH_String(Reply.Text(item, "name")), path);
                        values.Append(new GH_String(Reply.Text(item, "value")), path);
                        statuses.Append(new GH_String(status), path);
                        groups.Append(new GH_String(Reply.Text(item, "group")), path);
                        if (!string.Equals(status, "hasValue", StringComparison.Ordinal))
                        {
                            unevaluated++;
                        }
                    }
                }
            }

            DA.SetDataList(0, found);
            DA.SetDataList(1, types);
            DA.SetDataList(2, ids);
            DA.SetDataList(3, layers);
            DA.SetDataList(4, stories);
            DA.SetDataList(5, levels);
            DA.SetDataTree(6, classifications);
            DA.SetDataTree(7, names);
            DA.SetDataTree(8, values);
            DA.SetDataTree(9, statuses);
            DA.SetDataTree(10, groups);

            if (hits < records.Count)
            {
                AddRuntimeMessage(
                    GH_RuntimeMessageLevel.Remark, "Resolved " + Reply.Summarise(hits, records.Count) + " elements.");
            }

            if (unevaluated > 0)
            {
                // A remark, not a warning: a built-in that does not apply to a
                // wall is Archicad being correct, not a problem with the
                // definition. Said at all because a blank Value column otherwise
                // looks like a failed read.
                AddRuntimeMessage(
                    GH_RuntimeMessageLevel.Remark,
                    unevaluated.ToString(System.Globalization.CultureInfo.InvariantCulture)
                    + " propert(ies) have no value for these elements - read Status to see which are not applicable "
                    + "and which Archicad could not evaluate.");
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
}
