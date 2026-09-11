using System;
using System.Collections;
using System.Collections.Generic;
using System.Text.Json;
using System.Windows.Forms;

using GH_IO.Serialization;
using Grasshopper.Kernel;
using Grasshopper.Kernel.Parameters;
using Grasshopper.Kernel.Types;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// An Archicad element selection, chosen in Archicad and injected before the
    /// solve. Many values, not one.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ IT EXISTS BECAUSE A COMPONENT THAT READS THE SELECTION CANNOT SERVE A
    /// HEADLESS RUN. Tapioca Selection asks Archicad what is selected right now,
    /// which is what you want while building a definition with the canvas in
    /// front of you. A definition driven from the Tapioca panel has no canvas and
    /// nobody watching it: the elements have to arrive the way every other input
    /// arrives — pushed in by the panel, before the solve, as part of the
    /// request. That is what this parameter is: the SELECTION AS AN INPUT.
    /// </para>
    /// <para>
    /// ⚠️ THE PANEL'S SELECTION CONTROLS ARE THE UI, AND THEY ALREADY EXIST.
    /// A Tapioca command declares <c>selection_sets=("Elements",)</c> and the
    /// palette builds a row of Update / Add / Remove / Reselect / Clear buttons
    /// for it — the row the UI Showcase command shows. The Grasshopper command
    /// now declares one, so a workflow gets the same controls, and the guids they
    /// hold are sent to the input whose <see cref="Role"/> matches the set's
    /// name. Nothing about that UI is reinvented here.
    /// </para>
    /// <para>
    /// ⚠️ AND THE SAME FOUR ACTIONS ARE ON THIS PARAMETER'S CONTEXT MENU, because
    /// a definition is also built at the canvas, where Archicad's panel is behind
    /// a window. Right-click gives Update / Add / Remove / Reselect against
    /// Archicad's live selection over the bridge — the same verbs, the same
    /// meanings, reached the way Grasshopper reaches everything else. (This is
    /// how GRAPHISOFT's own element parameters do it: a "Get …" menu item rather
    /// than buttons on the canvas.)
    /// </para>
    /// <para>
    /// ⚠️ THE VALUE ON THE WIRE IS NEWLINE-SEPARATED GUIDS, ONE STRING. Every
    /// session input value is a string, and that contract is not worth breaking
    /// for one type: a list would have meant a second value shape in the
    /// protocol, in the schema, in the panel's snapshot and in the worker's
    /// writer. Splitting one string is the whole cost, and it is paid here.
    /// </para>
    /// </remarks>
    public class TapiocaSelectionParam : Param_String, IGH_ContextualParameter, ITapiocaInput
    {
        private readonly TapiocaInputCore m_core = new TapiocaInputCore("selection");

        /// <summary>
        /// The character the panel joins guids with, and this splits on.
        /// </summary>
        /// <remarks>
        /// A newline rather than a comma or a semicolon: a GUID can contain
        /// neither, but so can a great many things, and a separator that is
        /// visible when a value is printed into the transcript is worth more than
        /// one that is compact.
        /// </remarks>
        internal const char Separator = '\n';

        public TapiocaSelectionParam()
        {
            Name = "Tapioca Selection";
            NickName = "Sel";
            Description = "Archicad elements Tapioca collects in the panel and injects before the solve, as GUIDs.";
            Category = "Tapioca";
            SubCategory = "Inputs";
            Access = GH_ParamAccess.list;
        }

        internal TapiocaInputCore Core
        {
            get { return m_core; }
        }

        /// <summary>
        /// Which of the command's declared selection sets fills this parameter.
        /// </summary>
        /// <remarks>
        /// The nickname, so that naming the parameter "Elements" is all it takes
        /// to bind it to the set called "Elements" — the same rule every other
        /// Tapioca input follows for its id, and one less concept.
        /// </remarks>
        public string Role
        {
            get { return m_core.EffectiveId(NickName); }
        }

        public override Guid ComponentGuid
        {
            get { return new Guid("7d3e5c92-14b8-4a06-9f2d-8b45e7106c3a"); }
        }

        public override GH_Exposure Exposure
        {
            get { return GH_Exposure.primary; }
        }

        public override string Name
        {
            get { return m_core.EffectiveId(base.NickName); }
            set
            {
                base.Name = value;
                m_core.Id = value;
            }
        }

        public string TapiocaTypeName { get { return m_core.TypeName; } }

        public string TapiocaId { get { return m_core.EffectiveId(NickName); } }

        public string TapiocaLabel { get { return m_core.EffectiveLabel(NickName); } }

        public string TapiocaGroup { get { return m_core.Group; } }

        public int TapiocaOrder { get { return m_core.Order; } }

        public bool TapiocaRequired { get { return m_core.Required; } }

        public double? TapiocaMinimum { get { return null; } }

        public double? TapiocaMaximum { get { return null; } }

        public IList<string> TapiocaChoices { get { return m_core.Choices; } }

        /// <summary>
        /// Every held guid, joined — what the panel reads back to show what this
        /// input currently has.
        /// </summary>
        public string TapiocaCurrentValue
        {
            get
            {
                List<string> guids = new List<string>();
                foreach (GH_String text in PersistentData.AllData(true))
                {
                    if (text != null && !string.IsNullOrWhiteSpace(text.Value))
                    {
                        guids.Add(text.Value.Trim());
                    }
                }

                return string.Join(Separator.ToString(), guids);
            }
        }

        public string Prompt
        {
            get { return m_core.EffectiveLabel(NickName); }
        }

        public int AtLeast
        {
            get { return m_core.Required ? 1 : 0; }
        }

        /// <summary>
        /// ⚠️ MANY, UNLIKE EVERY OTHER TAPIOCA INPUT. A selection of one is a
        /// special case of a selection, not the shape of one.
        /// </summary>
        public int AtMost
        {
            get { return int.MaxValue; }
        }

        public bool Immediate
        {
            get { return false; }
        }

        public IEnumerable<object> ContextualData
        {
            get
            {
                List<object> values = new List<object>();
                foreach (GH_String text in PersistentData.AllData(true))
                {
                    if (text != null)
                    {
                        values.Add(text.Value);
                    }
                }

                return values;
            }
        }

        public void AssignContextualData(IEnumerable data)
        {
            TapiocaInputGuard.NoteIfDriven(this);
            SetGuids(Expand(data));
        }

        public void AssignContextualDataTree(global::Grasshopper.DataTree<GH_String> tree)
        {
            TapiocaInputGuard.NoteIfDriven(this);

            List<object> raw = new List<object>();
            foreach (GH_String text in TapiocaInputGuard.Flatten(tree))
            {
                if (text != null)
                {
                    raw.Add(text.Value);
                }
            }

            SetGuids(Expand(raw));
        }

        public void ClearContextualData()
        {
            m_fromPanel = null;
            PersistentData.Clear();
            ExpireSolution(false);
        }

        public bool AutoAssignContextualData(GH_ParameterContext context)
        {
            return true;
        }

        /// <summary>
        /// One joined string, or several already-separate ones, to a guid list.
        /// </summary>
        /// <remarks>
        /// ⚠️ BOTH SHAPES ARRIVE AND BOTH ARE MEANT. The panel sends one value
        /// with newlines in it; a wired Grasshopper source sends one item per
        /// guid; and rhino.compute's data-tree path can send either. Splitting
        /// unconditionally is what makes all three the same thing by the time
        /// they reach the parameter.
        /// </remarks>
        private static List<string> Expand(IEnumerable data)
        {
            List<string> guids = new List<string>();
            if (data == null)
            {
                return guids;
            }

            foreach (object raw in data)
            {
                string value;
                if (!TapiocaInputCore.TryToString(raw, out value) || value == null)
                {
                    continue;
                }

                string[] parts = value.Split(new char[] { Separator, '\r' }, StringSplitOptions.RemoveEmptyEntries);
                for (int index = 0; index < parts.Length; index++)
                {
                    string guid = parts[index].Trim();
                    if (guid.Length > 0)
                    {
                        guids.Add(guid);
                    }
                }
            }

            return guids;
        }

        private void SetGuids(List<string> guids)
        {
            List<GH_String> values = new List<GH_String>();
            for (int index = 0; index < guids.Count; index++)
            {
                values.Add(new GH_String(guids[index]));
            }

            m_fromPanel = values;
            SetPersistentData(values);
            ExpireSolution(false);
        }

        // ---- the same four verbs, on the canvas ----------------------------

        /// <remarks>
        /// ⚠️ THE MENU TALKS TO ARCHICAD, NOT TO THE PANEL. These run while
        /// somebody is building a definition, so they read and write Archicad's
        /// LIVE selection over the bridge (Tapioca.GetSelection,
        /// Tapioca.ModifySelection) — the same commands Tapioca Selection and
        /// Tapioca Select use. The panel's own selection sets are what fill this
        /// parameter during a headless run; these are for the canvas, and the two
        /// do not need to agree at the same instant.
        /// </remarks>
        public override void AppendAdditionalMenuItems(ToolStripDropDown menu)
        {
            base.AppendAdditionalMenuItems(menu);

            Menu_AppendSeparator(menu);
            Menu_AppendItem(menu, "Update from Archicad selection", OnUpdate);
            Menu_AppendItem(menu, "Add Archicad selection", OnAdd);
            Menu_AppendItem(menu, "Remove Archicad selection", OnRemove);
            Menu_AppendItem(menu, "Reselect in Archicad", OnReselect);
            Menu_AppendItem(menu, "Clear", OnClearSelection);
        }

        private void OnUpdate(object sender, EventArgs args)
        {
            List<string> read = ReadArchicadSelection();
            if (read != null)
            {
                SetGuids(read);
            }
        }

        private void OnAdd(object sender, EventArgs args)
        {
            List<string> read = ReadArchicadSelection();
            if (read == null)
            {
                return;
            }

            // Set semantics, and deliberately: adding the same element twice
            // would make it solve twice, which no caller means by "add".
            List<string> merged = new List<string>();
            HashSet<string> seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (GH_String text in PersistentData.AllData(true))
            {
                if (text != null && !string.IsNullOrWhiteSpace(text.Value) && seen.Add(text.Value.Trim()))
                {
                    merged.Add(text.Value.Trim());
                }
            }

            for (int index = 0; index < read.Count; index++)
            {
                if (seen.Add(read[index]))
                {
                    merged.Add(read[index]);
                }
            }

            SetGuids(merged);
        }

        private void OnRemove(object sender, EventArgs args)
        {
            List<string> read = ReadArchicadSelection();
            if (read == null)
            {
                return;
            }

            HashSet<string> drop = new HashSet<string>(read, StringComparer.OrdinalIgnoreCase);
            List<string> kept = new List<string>();
            foreach (GH_String text in PersistentData.AllData(true))
            {
                if (text == null || string.IsNullOrWhiteSpace(text.Value))
                {
                    continue;
                }

                string guid = text.Value.Trim();
                if (!drop.Contains(guid))
                {
                    kept.Add(guid);
                }
            }

            SetGuids(kept);
        }

        /// <remarks>
        /// The one verb that writes: it puts what this parameter holds back into
        /// Archicad's selection, which is how a user sees WHICH elements a
        /// definition is about. Selecting is not a model change — see the note in
        /// Tapioca Select — so it is allowed over the read-only bridge.
        /// </remarks>
        private void OnReselect(object sender, EventArgs args)
        {
            List<string> guids = new List<string>();
            foreach (GH_String text in PersistentData.AllData(true))
            {
                if (text != null && !string.IsNullOrWhiteSpace(text.Value))
                {
                    guids.Add(text.Value.Trim());
                }
            }

            if (guids.Count == 0)
            {
                return;
            }

            string elements = Reply.ElementsRequest(guids);
            string parameters = "{\"op\":\"replace\"," + elements.Substring(1, elements.Length - 2) + "}";
            TapiocaBridge.Call("Tapioca.ModifySelection", parameters);
        }

        private void OnClearSelection(object sender, EventArgs args)
        {
            SetGuids(new List<string>());
        }

        /// <summary>
        /// Archicad's current selection, or null when it could not be read.
        /// </summary>
        /// <remarks>
        /// Null rather than an empty list, because the two mean different things
        /// to Add and Remove: "nothing is selected" is a legitimate answer that
        /// changes nothing, while "the bridge is not there" must not be mistaken
        /// for it.
        /// </remarks>
        private List<string> ReadArchicadSelection()
        {
            string response = TapiocaBridge.Call("Tapioca.GetSelection", string.Empty);
            if (!Envelope.IsOk(response))
            {
                return null;
            }

            List<string> guids = new List<string>();
            IList<JsonElement> records = Reply.Records(Envelope.DataOf(response), "elements");
            for (int index = 0; index < records.Count; index++)
            {
                string guid = Reply.GuidOf(records[index]);
                if (guid.Length > 0)
                {
                    guids.Add(guid);
                }
            }

            return guids;
        }

        // the panel's value, over a wire ------------------------------------

        private List<GH_String> m_fromPanel;

        protected override void CollectVolatileData_FromSources()
        {
            base.CollectVolatileData_FromSources();
            TapiocaInputGuard.ApplyOverride(this, m_fromPanel);
        }

        public override bool Write(GH_IWriter writer)
        {
            m_core.Write(writer);
            return base.Write(writer);
        }

        public override bool Read(GH_IReader reader)
        {
            m_core.Read(reader);
            return base.Read(reader);
        }
    }
}
