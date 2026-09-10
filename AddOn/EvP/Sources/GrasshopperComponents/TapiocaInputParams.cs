using System;
using System.Collections;
using System.Collections.Generic;

using GH_IO.Serialization;
using Grasshopper.Kernel;
using Grasshopper.Kernel.Data;
using Grasshopper.Kernel.Parameters;
using Grasshopper.Kernel.Special;
using Grasshopper.Kernel.Types;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// What discovery looks for. Implemented by every Tapioca input parameter.
    /// </summary>
    /// <remarks>
    /// Public because the worker binds to this package by reflection and never
    /// by assembly reference — see Tapioca.Grasshopper.csproj on why a
    /// compile-time reference between the two would resolve a second copy of
    /// the worker into another load context.
    /// </remarks>
    public interface ITapiocaInput
    {
        /// <summary>The schema type name: number, integer, boolean, string, enum.</summary>
        string TapiocaTypeName { get; }

        /// <summary>The stable id the Archicad panel keys its control by.</summary>
        string TapiocaId { get; }

        string TapiocaLabel { get; }

        string TapiocaGroup { get; }

        /// <summary>Explicit order within the group; negative means unset.</summary>
        int TapiocaOrder { get; }

        bool TapiocaRequired { get; }

        double? TapiocaMinimum { get; }

        double? TapiocaMaximum { get; }

        /// <summary>Enum rows in author order; empty for every other type.</summary>
        IList<string> TapiocaChoices { get; }

        /// <summary>The current value, as the schema would express a default.</summary>
        string TapiocaCurrentValue { get; }
    }

    /// <summary>
    /// Refuses injection into a parameter that something else already drives.
    /// </summary>
    /// <remarks>
    /// ⚠️ THIS TURNS THE QUIETEST FAILURE ON THIS PATH INTO A VISIBLE ONE.
    /// Contextual injection writes PERSISTENT data, and Grasshopper reads
    /// persistent data only when a parameter has NO SOURCE. Wire a slider or a
    /// panel into a workflow input and every value Tapioca sends is discarded —
    /// while the solve still SUCCEEDS, returning results computed from the wired
    /// value. Nothing in the response says so.
    ///
    /// Measured twice on one definition: number sliders wired into the numeric
    /// inputs, then a panel wired into a text input, each producing a clean
    /// HTTP 200 whose output never changed no matter what was injected.
    ///
    /// A workflow input IS the slider's replacement. Saying so here costs one
    /// check and saves the hour it otherwise takes to find.
    /// </remarks>
    internal static class TapiocaInputGuard
    {
        /// <summary>
        /// The write path rhino.compute actually uses.
        /// </summary>
        /// <remarks>
        /// ⚠️ COMPUTE DOES NOT CALL <c>IGH_ContextualParameter.AssignContextualData</c>.
        /// It resolves a method literally named "AssignContextualDataTree" by
        /// REFLECTION on the concrete parameter type and invokes it with a
        /// <c>Grasshopper.DataTree&lt;T&gt;</c> — see GrasshopperDefinition.cs,
        /// BuildAndAssignContextualTree, in compute.rhino3d. The call site is
        /// <c>method?.Invoke(...)</c>, so a parameter that does not declare the
        /// method is SKIPPED IN SILENCE: no error, no warning, and a solve that
        /// succeeds against an empty parameter.
        ///
        /// That cost this task a full session of measurement. Implementing only
        /// the published interface is not enough to be a compute input, and
        /// nothing in the API or the guides says so.
        ///
        /// Volatile data rather than persistent, mirroring what compute does for
        /// its non-contextual inputs one branch above the reflection call:
        /// ClearData, expire without recomputing, then add. The solution runs as
        /// NewSolution(false, ...), so data staged here survives into it.
        /// </remarks>
        internal static void AssignTree<T> (IGH_Param param, global::Grasshopper.DataTree<T> tree)
        {
            param.ClearData ();
            param.ExpireSolution (false);

            if (tree == null)
            {
                return;
            }

            foreach (GH_Path path in tree.Paths)
            {
                List<T> branch = tree.Branch (path);
                if (branch != null && branch.Count > 0)
                {
                    param.AddVolatileDataList (path, branch);
                }
            }
        }

        /// <summary>
        /// Notes that a wired input is DOCUMENTED by its wire and DRIVEN by the
        /// panel. Returns whether there was a wire at all.
        /// </summary>
        /// <remarks>
        /// <para>
        /// ⚠️ THIS USED TO REFUSE THE ASSIGNMENT, AND REFUSING WAS THE WRONG
        /// ANSWER. Grasshopper reads a parameter's persistent data only when it
        /// has NO source, so a wired input genuinely could not be injected --
        /// which is where the old warning came from ("disconnect what feeds
        /// it"). But an author who wires a number slider into a workflow input
        /// is saying something useful: this input is a number between these two
        /// bounds, and here is a sensible one. Throwing that away to protect a
        /// rule about persistent data is refusing the author's own
        /// specification.
        /// </para>
        /// <para>
        /// So the wire is now a SPECIFICATION and the panel is the VALUE. The
        /// domain comes off the wired slider (or the choices off a wired value
        /// list) when the author set none explicitly, and the panel's value
        /// wins at solve time through <see cref="ApplyOverride"/> -- which is
        /// only possible because these are OUR parameter classes and can
        /// override their own collection. A stock Get-parameter cannot do this,
        /// and that difference is now the reason to prefer a Tapioca input.
        /// </para>
        /// </remarks>
        /// <summary>
        /// The bounds a single wired number slider states, or null.
        /// </summary>
        /// <remarks>
        /// ⚠️ ONE SOURCE, AND ONLY A SLIDER. Two sources have no single domain,
        /// and anything else wired in -- an expression, another parameter, a
        /// series -- states no bounds at all, so guessing from it would invent
        /// a limit the author never set. compute reads a wired slider the same
        /// way for the same reason (GrasshopperDefinition.cs,
        /// InputGroup.GetMinimum).
        /// </remarks>
        internal static double? WiredMinimum (IGH_Param param)
        {
            GH_NumberSlider slider = WiredSlider (param);
            return slider == null ? (double?) null : (double) slider.Slider.Minimum;
        }

        internal static double? WiredMaximum (IGH_Param param)
        {
            GH_NumberSlider slider = WiredSlider (param);
            return slider == null ? (double?) null : (double) slider.Slider.Maximum;
        }

        private static GH_NumberSlider WiredSlider (IGH_Param param)
        {
            if (param.SourceCount != 1)
            {
                return null;
            }

            return param.Sources[0] as GH_NumberSlider;
        }

        /// <summary>
        /// The choices a single wired value list states, in its own order.
        /// </summary>
        /// <remarks>
        /// The enum equivalent of a wired slider: an author who plugs a value
        /// list into a Tapioca enum has already written the pick list, and
        /// making them type it twice is friction with no benefit.
        /// </remarks>
        internal static List<string> WiredChoices (IGH_Param param)
        {
            List<string> choices = new List<string> ();
            if (param.SourceCount != 1)
            {
                return choices;
            }

            GH_ValueList list = param.Sources[0] as GH_ValueList;
            if (list == null)
            {
                return choices;
            }

            foreach (GH_ValueListItem item in list.ListItems)
            {
                if (item != null && !string.IsNullOrWhiteSpace (item.Name))
                {
                    choices.Add (item.Name);
                }
            }

            return choices;
        }

        internal static bool NoteIfDriven (IGH_Param param)
        {
            if (param.SourceCount == 0)
            {
                return false;
            }

            param.AddRuntimeMessage (
                GH_RuntimeMessageLevel.Remark,
                "Tapioca drives this input. What is wired into it defines its domain and its "
                    + "starting value; the value used in a solve is the one the Archicad panel sends.");

            return true;
        }

        /// <summary>
        /// Replaces whatever was collected from sources with the panel's value.
        /// </summary>
        /// <remarks>
        /// ⚠️ CALLED FROM CollectVolatileData_FromSources, WHICH IS THE ONLY
        /// PLACE IT CAN WORK. Grasshopper collects from sources on every
        /// solution; anything written before that -- persistent data, volatile
        /// data, either -- is overwritten by the collection when a source
        /// exists. Overriding the collection itself is what lets a wired input
        /// still take a value from outside the document, and it is available
        /// because these are our own parameter classes.
        ///
        /// A null or empty override leaves the collected data alone: no
        /// assignment has been made, so the wire is the only opinion there is.
        /// </remarks>
        /// <summary>
        /// One flat list out of a contextual data tree.
        /// </summary>
        /// <remarks>
        /// The panel sends one value per input (AtMost is 1 on every one of
        /// these), so a tree is the transport's shape and not the data's.
        /// Flattening it here keeps the override a plain list, which is what the
        /// collection override writes back at path {0}.
        /// </remarks>
        internal static List<T> Flatten<T> (global::Grasshopper.DataTree<T> tree)
        {
            List<T> flat = new List<T> ();
            if (tree == null)
            {
                return flat;
            }

            foreach (GH_Path path in tree.Paths)
            {
                List<T> branch = tree.Branch (path);
                if (branch != null)
                {
                    flat.AddRange (branch);
                }
            }

            return flat;
        }

        internal static void ApplyOverride<T> (IGH_Param param, List<T> values) where T : IGH_Goo
        {
            if (values == null || values.Count == 0 || param.SourceCount == 0)
            {
                return;
            }

            param.VolatileData.Clear ();
            param.AddVolatileDataList (new GH_Path (0), values);
        }
    }

    /// <summary>
    /// A floating point input, exposed to Tapioca's panel as a number.
    /// </summary>
    /// <remarks>
    /// ⚠️ THE CONTEXTUAL INTERFACE IS THE WHOLE POINT OF THIS TYPE, not the
    /// Param_Number it derives from. rhino.compute discovers and injects it, and
    /// so does the in-process worker, because both drive
    /// IGH_ContextualParameter. Dropping the interface would leave a parameter
    /// that looks right on the canvas and is invisible to every backend.
    /// </remarks>
    public class TapiocaNumberParam : Param_Number, IGH_ContextualParameter, ITapiocaInput
    {
        private readonly TapiocaInputCore m_core = new TapiocaInputCore ("number");

        public TapiocaNumberParam ()
        {
            Name = "Tapioca Number";
            NickName = "Num";
            Description = "A number Tapioca collects in Archicad and injects before the solve.";
            Category = "Tapioca";
            SubCategory = "Inputs";
        }

        internal TapiocaInputCore Core
        {
            get { return m_core; }
        }

        public override Guid ComponentGuid
        {
            get { return new Guid("3f1c9a02-6d47-4b18-9e5a-2c8b7d04e611"); }
        }

        public override GH_Exposure Exposure
        {
            get { return GH_Exposure.primary; }
        }

        // See TapiocaInputCore: identity is the parameter Name because compute
        // keys inputs by it, so a rename on the canvas IS an id change and must
        // not silently diverge from one.
        public override string Name
        {
            get { return m_core.EffectiveId (base.NickName); }
            set
            {
                base.Name = value;
                m_core.Id = value;
            }
        }

        public string TapiocaTypeName { get { return m_core.TypeName; } }

        public string TapiocaId { get { return m_core.EffectiveId (NickName); } }

        public string TapiocaLabel { get { return m_core.EffectiveLabel (NickName); } }

        public string TapiocaGroup { get { return m_core.Group; } }

        public int TapiocaOrder { get { return m_core.Order; } }

        public bool TapiocaRequired { get { return m_core.Required; } }

        // ⚠️ THE WIRE IS READ ONLY WHERE THE AUTHOR TYPED NOTHING. An explicit
        // bound is a decision and must win over one inferred from whatever
        // happens to be plugged in; an absent bound is an opportunity.
        public double? TapiocaMinimum
        {
            get { return m_core.Minimum ?? TapiocaInputGuard.WiredMinimum (this); }
        }

        public double? TapiocaMaximum
        {
            get { return m_core.Maximum ?? TapiocaInputGuard.WiredMaximum (this); }
        }

        public IList<string> TapiocaChoices { get { return m_core.Choices; } }

        public string TapiocaCurrentValue
        {
            get
            {
                return PersistentData.DataCount > 0 && PersistentData.get_FirstItem (true) != null
                    ? PersistentData.get_FirstItem (true).Value.ToString (
                        System.Globalization.CultureInfo.InvariantCulture)
                    : string.Empty;
            }
        }

        // ── IGH_ContextualParameter ──────────────────────────────────────────

        public string Prompt
        {
            get { return m_core.EffectiveLabel (NickName); }
        }

        public int AtLeast
        {
            get { return m_core.Required ? 1 : 0; }
        }

        public int AtMost
        {
            get { return 1; }
        }

        public bool Immediate
        {
            get { return false; }
        }

        public IEnumerable<object> ContextualData
        {
            get
            {
                List<object> values = new List<object> ();
                foreach (GH_Number number in PersistentData.AllData (true))
                {
                    if (number != null)
                    {
                        values.Add (number.Value);
                    }
                }

                return values;
            }
        }

        public void AssignContextualData (IEnumerable data)
        {
            TapiocaInputGuard.NoteIfDriven (this);

            List<GH_Number> values = new List<GH_Number> ();

            if (data != null)
            {
                foreach (object raw in data)
                {
                    double value;
                    if (TapiocaInputCore.TryToDouble (raw, out value))
                    {
                        values.Add (new GH_Number (m_core.Clamp (value)));
                    }
                }
            }

            m_fromPanel = values;
            SetPersistentData (values);
            ExpireSolution (false);
        }

        // The method rhino.compute invokes by reflection. See
        // TapiocaInputGuard.AssignTree for why the interface alone is not enough.
        public void AssignContextualDataTree (global::Grasshopper.DataTree<GH_Number> tree)
        {
            TapiocaInputGuard.NoteIfDriven (this);

            m_fromPanel = TapiocaInputGuard.Flatten (tree);
            TapiocaInputGuard.AssignTree (this, tree);
        }

        public void ClearContextualData ()
        {
            m_fromPanel = null;
            PersistentData.Clear ();
            ExpireSolution (false);
        }

        // Never prompts. A headless solve has nobody to ask, and returning false
        // would abort the whole solution rather than leave one input at its
        // saved value — see SPEC-RhinoCompute.md.
        public bool AutoAssignContextualData (GH_ParameterContext context)
        {
            return true;
        }

        // ── persistence ──────────────────────────────────────────────────────

        // the panel's value, over a wire ------------------------------------

        /// <summary>What the panel last sent, or null.</summary>
        /// <remarks>
        /// Kept BESIDE the persistent data rather than instead of it: with no
        /// source, persistent data is what Grasshopper reads and that proven
        /// path stays untouched. This is consulted only when a source exists --
        /// see TapiocaInputGuard.ApplyOverride for why a parameter's own
        /// collection is the one place it can be applied.
        /// </remarks>
        private List<GH_Number> m_fromPanel;

        protected override void CollectVolatileData_FromSources ()
        {
            base.CollectVolatileData_FromSources ();
            TapiocaInputGuard.ApplyOverride (this, m_fromPanel);
        }

        public override bool Write (GH_IWriter writer)
        {
            m_core.Write (writer);
            return base.Write (writer);
        }

        public override bool Read (GH_IReader reader)
        {
            m_core.Read (reader);
            return base.Read (reader);
        }
    }

    /// <summary>
    /// A whole-number input, exposed to Tapioca's panel as an integer.
    /// </summary>
    public class TapiocaIntegerParam : Param_Integer, IGH_ContextualParameter, ITapiocaInput
    {
        private readonly TapiocaInputCore m_core = new TapiocaInputCore ("integer");

        public TapiocaIntegerParam ()
        {
            Name = "Tapioca Integer";
            NickName = "Int";
            Description = "A whole number Tapioca collects in Archicad and injects before the solve.";
            Category = "Tapioca";
            SubCategory = "Inputs";
        }

        internal TapiocaInputCore Core
        {
            get { return m_core; }
        }

        public override Guid ComponentGuid
        {
            get { return new Guid("7b25d4e8-0af3-4c61-8d92-5e1a63b70c44"); }
        }

        public override GH_Exposure Exposure
        {
            get { return GH_Exposure.primary; }
        }

        public override string Name
        {
            get { return m_core.EffectiveId (base.NickName); }
            set
            {
                base.Name = value;
                m_core.Id = value;
            }
        }

        public string TapiocaTypeName { get { return m_core.TypeName; } }

        public string TapiocaId { get { return m_core.EffectiveId (NickName); } }

        public string TapiocaLabel { get { return m_core.EffectiveLabel (NickName); } }

        public string TapiocaGroup { get { return m_core.Group; } }

        public int TapiocaOrder { get { return m_core.Order; } }

        public bool TapiocaRequired { get { return m_core.Required; } }

        // ⚠️ THE WIRE IS READ ONLY WHERE THE AUTHOR TYPED NOTHING. An explicit
        // bound is a decision and must win over one inferred from whatever
        // happens to be plugged in; an absent bound is an opportunity.
        public double? TapiocaMinimum
        {
            get { return m_core.Minimum ?? TapiocaInputGuard.WiredMinimum (this); }
        }

        public double? TapiocaMaximum
        {
            get { return m_core.Maximum ?? TapiocaInputGuard.WiredMaximum (this); }
        }

        public IList<string> TapiocaChoices { get { return m_core.Choices; } }

        public string TapiocaCurrentValue
        {
            get
            {
                return PersistentData.DataCount > 0 && PersistentData.get_FirstItem (true) != null
                    ? PersistentData.get_FirstItem (true).Value.ToString (
                        System.Globalization.CultureInfo.InvariantCulture)
                    : string.Empty;
            }
        }

        public string Prompt
        {
            get { return m_core.EffectiveLabel (NickName); }
        }

        public int AtLeast
        {
            get { return m_core.Required ? 1 : 0; }
        }

        public int AtMost
        {
            get { return 1; }
        }

        public bool Immediate
        {
            get { return false; }
        }

        public IEnumerable<object> ContextualData
        {
            get
            {
                List<object> values = new List<object> ();
                foreach (GH_Integer number in PersistentData.AllData (true))
                {
                    if (number != null)
                    {
                        values.Add (number.Value);
                    }
                }

                return values;
            }
        }

        public void AssignContextualData (IEnumerable data)
        {
            TapiocaInputGuard.NoteIfDriven (this);

            List<GH_Integer> values = new List<GH_Integer> ();

            if (data != null)
            {
                foreach (object raw in data)
                {
                    int value;
                    if (TapiocaInputCore.TryToInt (raw, out value))
                    {
                        values.Add (new GH_Integer ((int) m_core.Clamp (value)));
                    }
                }
            }

            m_fromPanel = values;
            SetPersistentData (values);
            ExpireSolution (false);
        }

        // The method rhino.compute invokes by reflection. See
        // TapiocaInputGuard.AssignTree for why the interface alone is not enough.
        public void AssignContextualDataTree (global::Grasshopper.DataTree<GH_Integer> tree)
        {
            TapiocaInputGuard.NoteIfDriven (this);

            m_fromPanel = TapiocaInputGuard.Flatten (tree);
            TapiocaInputGuard.AssignTree (this, tree);
        }

        public void ClearContextualData ()
        {
            m_fromPanel = null;
            PersistentData.Clear ();
            ExpireSolution (false);
        }

        public bool AutoAssignContextualData (GH_ParameterContext context)
        {
            return true;
        }

        // the panel's value, over a wire ------------------------------------

        /// <summary>What the panel last sent, or null.</summary>
        /// <remarks>
        /// Kept BESIDE the persistent data rather than instead of it: with no
        /// source, persistent data is what Grasshopper reads and that proven
        /// path stays untouched. This is consulted only when a source exists --
        /// see TapiocaInputGuard.ApplyOverride for why a parameter's own
        /// collection is the one place it can be applied.
        /// </remarks>
        private List<GH_Integer> m_fromPanel;

        protected override void CollectVolatileData_FromSources ()
        {
            base.CollectVolatileData_FromSources ();
            TapiocaInputGuard.ApplyOverride (this, m_fromPanel);
        }

        public override bool Write (GH_IWriter writer)
        {
            m_core.Write (writer);
            return base.Write (writer);
        }

        public override bool Read (GH_IReader reader)
        {
            m_core.Read (reader);
            return base.Read (reader);
        }
    }

    /// <summary>
    /// A true/false input, exposed to Tapioca's panel as a checkbox.
    /// </summary>
    public class TapiocaBooleanParam : Param_Boolean, IGH_ContextualParameter, ITapiocaInput
    {
        private readonly TapiocaInputCore m_core = new TapiocaInputCore ("boolean");

        public TapiocaBooleanParam ()
        {
            Name = "Tapioca Boolean";
            NickName = "Bool";
            Description = "A true/false switch Tapioca collects in Archicad and injects before the solve.";
            Category = "Tapioca";
            SubCategory = "Inputs";
        }

        internal TapiocaInputCore Core
        {
            get { return m_core; }
        }

        public override Guid ComponentGuid
        {
            get { return new Guid("c4a70e19-8b36-4d52-a1f7-90e2d5c8b703"); }
        }

        public override GH_Exposure Exposure
        {
            get { return GH_Exposure.primary; }
        }

        public override string Name
        {
            get { return m_core.EffectiveId (base.NickName); }
            set
            {
                base.Name = value;
                m_core.Id = value;
            }
        }

        public string TapiocaTypeName { get { return m_core.TypeName; } }

        public string TapiocaId { get { return m_core.EffectiveId (NickName); } }

        public string TapiocaLabel { get { return m_core.EffectiveLabel (NickName); } }

        public string TapiocaGroup { get { return m_core.Group; } }

        public int TapiocaOrder { get { return m_core.Order; } }

        public bool TapiocaRequired { get { return m_core.Required; } }

        // A checkbox has no range. Reporting one would make the panel build a
        // spinner for something with two states.
        public double? TapiocaMinimum { get { return null; } }

        public double? TapiocaMaximum { get { return null; } }

        public IList<string> TapiocaChoices { get { return m_core.Choices; } }

        public string TapiocaCurrentValue
        {
            get
            {
                return PersistentData.DataCount > 0 && PersistentData.get_FirstItem (true) != null
                    ? (PersistentData.get_FirstItem (true).Value ? "true" : "false")
                    : string.Empty;
            }
        }

        public string Prompt
        {
            get { return m_core.EffectiveLabel (NickName); }
        }

        public int AtLeast
        {
            get { return m_core.Required ? 1 : 0; }
        }

        public int AtMost
        {
            get { return 1; }
        }

        public bool Immediate
        {
            get { return false; }
        }

        public IEnumerable<object> ContextualData
        {
            get
            {
                List<object> values = new List<object> ();
                foreach (GH_Boolean flag in PersistentData.AllData (true))
                {
                    if (flag != null)
                    {
                        values.Add (flag.Value);
                    }
                }

                return values;
            }
        }

        public void AssignContextualData (IEnumerable data)
        {
            TapiocaInputGuard.NoteIfDriven (this);

            List<GH_Boolean> values = new List<GH_Boolean> ();

            if (data != null)
            {
                foreach (object raw in data)
                {
                    bool value;
                    if (TapiocaInputCore.TryToBool (raw, out value))
                    {
                        values.Add (new GH_Boolean (value));
                    }
                }
            }

            m_fromPanel = values;
            SetPersistentData (values);
            ExpireSolution (false);
        }

        // The method rhino.compute invokes by reflection. See
        // TapiocaInputGuard.AssignTree for why the interface alone is not enough.
        public void AssignContextualDataTree (global::Grasshopper.DataTree<GH_Boolean> tree)
        {
            TapiocaInputGuard.NoteIfDriven (this);

            m_fromPanel = TapiocaInputGuard.Flatten (tree);
            TapiocaInputGuard.AssignTree (this, tree);
        }

        public void ClearContextualData ()
        {
            m_fromPanel = null;
            PersistentData.Clear ();
            ExpireSolution (false);
        }

        public bool AutoAssignContextualData (GH_ParameterContext context)
        {
            return true;
        }

        // the panel's value, over a wire ------------------------------------

        /// <summary>What the panel last sent, or null.</summary>
        /// <remarks>
        /// Kept BESIDE the persistent data rather than instead of it: with no
        /// source, persistent data is what Grasshopper reads and that proven
        /// path stays untouched. This is consulted only when a source exists --
        /// see TapiocaInputGuard.ApplyOverride for why a parameter's own
        /// collection is the one place it can be applied.
        /// </remarks>
        private List<GH_Boolean> m_fromPanel;

        protected override void CollectVolatileData_FromSources ()
        {
            base.CollectVolatileData_FromSources ();
            TapiocaInputGuard.ApplyOverride (this, m_fromPanel);
        }

        public override bool Write (GH_IWriter writer)
        {
            m_core.Write (writer);
            return base.Write (writer);
        }

        public override bool Read (GH_IReader reader)
        {
            m_core.Read (reader);
            return base.Read (reader);
        }
    }

    /// <summary>
    /// A free-text input, exposed to Tapioca's panel as a text edit.
    /// </summary>
    public class TapiocaTextParam : Param_String, IGH_ContextualParameter, ITapiocaInput
    {
        private readonly TapiocaInputCore m_core = new TapiocaInputCore ("string");

        public TapiocaTextParam ()
        {
            Name = "Tapioca Text";
            NickName = "Txt";
            Description = "Text Tapioca collects in Archicad and injects before the solve.";
            Category = "Tapioca";
            SubCategory = "Inputs";
        }

        internal TapiocaInputCore Core
        {
            get { return m_core; }
        }

        public override Guid ComponentGuid
        {
            get { return new Guid("e0862b74-3d5f-41ca-b806-7f4c19a2d385"); }
        }

        public override GH_Exposure Exposure
        {
            get { return GH_Exposure.primary; }
        }

        public override string Name
        {
            get { return m_core.EffectiveId (base.NickName); }
            set
            {
                base.Name = value;
                m_core.Id = value;
            }
        }

        public string TapiocaTypeName { get { return m_core.TypeName; } }

        public string TapiocaId { get { return m_core.EffectiveId (NickName); } }

        public string TapiocaLabel { get { return m_core.EffectiveLabel (NickName); } }

        public string TapiocaGroup { get { return m_core.Group; } }

        public int TapiocaOrder { get { return m_core.Order; } }

        public bool TapiocaRequired { get { return m_core.Required; } }

        public double? TapiocaMinimum { get { return null; } }

        public double? TapiocaMaximum { get { return null; } }

        public IList<string> TapiocaChoices { get { return m_core.Choices; } }

        public string TapiocaCurrentValue
        {
            get
            {
                return PersistentData.DataCount > 0 && PersistentData.get_FirstItem (true) != null
                    ? PersistentData.get_FirstItem (true).Value
                    : string.Empty;
            }
        }

        public string Prompt
        {
            get { return m_core.EffectiveLabel (NickName); }
        }

        public int AtLeast
        {
            get { return m_core.Required ? 1 : 0; }
        }

        public int AtMost
        {
            get { return 1; }
        }

        public bool Immediate
        {
            get { return false; }
        }

        public IEnumerable<object> ContextualData
        {
            get
            {
                List<object> values = new List<object> ();
                foreach (GH_String text in PersistentData.AllData (true))
                {
                    if (text != null)
                    {
                        values.Add (text.Value);
                    }
                }

                return values;
            }
        }

        public void AssignContextualData (IEnumerable data)
        {
            TapiocaInputGuard.NoteIfDriven (this);

            List<GH_String> values = new List<GH_String> ();

            if (data != null)
            {
                foreach (object raw in data)
                {
                    string value;
                    if (TapiocaInputCore.TryToString (raw, out value))
                    {
                        values.Add (new GH_String (value));
                    }
                }
            }

            m_fromPanel = values;
            SetPersistentData (values);
            ExpireSolution (false);
        }

        // The method rhino.compute invokes by reflection. See
        // TapiocaInputGuard.AssignTree for why the interface alone is not enough.
        public void AssignContextualDataTree (global::Grasshopper.DataTree<GH_String> tree)
        {
            TapiocaInputGuard.NoteIfDriven (this);

            m_fromPanel = TapiocaInputGuard.Flatten (tree);
            TapiocaInputGuard.AssignTree (this, tree);
        }

        public void ClearContextualData ()
        {
            m_fromPanel = null;
            PersistentData.Clear ();
            ExpireSolution (false);
        }

        public bool AutoAssignContextualData (GH_ParameterContext context)
        {
            return true;
        }

        // the panel's value, over a wire ------------------------------------

        /// <summary>What the panel last sent, or null.</summary>
        /// <remarks>
        /// Kept BESIDE the persistent data rather than instead of it: with no
        /// source, persistent data is what Grasshopper reads and that proven
        /// path stays untouched. This is consulted only when a source exists --
        /// see TapiocaInputGuard.ApplyOverride for why a parameter's own
        /// collection is the one place it can be applied.
        /// </remarks>
        private List<GH_String> m_fromPanel;

        protected override void CollectVolatileData_FromSources ()
        {
            base.CollectVolatileData_FromSources ();
            TapiocaInputGuard.ApplyOverride (this, m_fromPanel);
        }

        public override bool Write (GH_IWriter writer)
        {
            m_core.Write (writer);
            return base.Write (writer);
        }

        public override bool Read (GH_IReader reader)
        {
            m_core.Read (reader);
            return base.Read (reader);
        }
    }

    /// <summary>
    /// A closed set of named choices, exposed to Tapioca's panel as a popup.
    /// </summary>
    /// <remarks>
    /// ⚠️ THE VALUE THAT CROSSES IS THE CHOICE TEXT, NOT ITS INDEX. An index
    /// would be smaller and is the wrong contract: inserting a row in the
    /// author's list would silently repoint every saved value after it, which is
    /// the same hazard ParamPanel's choiceValues exists to prevent on the
    /// Archicad side. A value that is no longer in the list is refused loudly
    /// rather than clamped to a neighbour.
    /// </remarks>
    public class TapiocaEnumParam : Param_String, IGH_ContextualParameter, ITapiocaInput
    {
        private readonly TapiocaInputCore m_core = new TapiocaInputCore ("enum");

        public TapiocaEnumParam ()
        {
            Name = "Tapioca Enum";
            NickName = "Enum";
            Description = "One of a fixed set of choices, presented in Archicad as a popup.";
            Category = "Tapioca";
            SubCategory = "Inputs";
        }

        internal TapiocaInputCore Core
        {
            get { return m_core; }
        }

        public override Guid ComponentGuid
        {
            get { return new Guid("a51f7c60-92d8-4e37-bb14-6c0a8e35f9d2"); }
        }

        public override GH_Exposure Exposure
        {
            get { return GH_Exposure.primary; }
        }

        public override string Name
        {
            get { return m_core.EffectiveId (base.NickName); }
            set
            {
                base.Name = value;
                m_core.Id = value;
            }
        }

        public string TapiocaTypeName { get { return m_core.TypeName; } }

        public string TapiocaId { get { return m_core.EffectiveId (NickName); } }

        public string TapiocaLabel { get { return m_core.EffectiveLabel (NickName); } }

        public string TapiocaGroup { get { return m_core.Group; } }

        public int TapiocaOrder { get { return m_core.Order; } }

        public bool TapiocaRequired { get { return m_core.Required; } }

        public double? TapiocaMinimum { get { return null; } }

        public double? TapiocaMaximum { get { return null; } }

        public IList<string> TapiocaChoices
        {
            get
            {
                // The author's own list wins; a wired value list fills in for
                // an enum that declares none, which is otherwise a row the
                // panel has to refuse.
                if (m_core.Choices.Count > 0)
                {
                    return m_core.Choices;
                }

                return TapiocaInputGuard.WiredChoices (this);
            }
        }

        public string TapiocaCurrentValue
        {
            get
            {
                return PersistentData.DataCount > 0 && PersistentData.get_FirstItem (true) != null
                    ? PersistentData.get_FirstItem (true).Value
                    : string.Empty;
            }
        }

        public string Prompt
        {
            get { return m_core.EffectiveLabel (NickName); }
        }

        public int AtLeast
        {
            get { return m_core.Required ? 1 : 0; }
        }

        public int AtMost
        {
            get { return 1; }
        }

        public bool Immediate
        {
            get { return false; }
        }

        public IEnumerable<object> ContextualData
        {
            get
            {
                List<object> values = new List<object> ();
                foreach (GH_String text in PersistentData.AllData (true))
                {
                    if (text != null)
                    {
                        values.Add (text.Value);
                    }
                }

                return values;
            }
        }

        public void AssignContextualData (IEnumerable data)
        {
            TapiocaInputGuard.NoteIfDriven (this);

            List<GH_String> values = new List<GH_String> ();
            List<string> rejected = new List<string> ();

            if (data != null)
            {
                foreach (object raw in data)
                {
                    string value;
                    if (!TapiocaInputCore.TryToString (raw, out value))
                    {
                        continue;
                    }

                    if (m_core.Choices.Count == 0 || m_core.Choices.Contains (value))
                    {
                        values.Add (new GH_String (value));
                    }
                    else
                    {
                        rejected.Add (value);
                    }
                }
            }

            m_fromPanel = values;
            SetPersistentData (values);

            if (rejected.Count > 0)
            {
                AddRuntimeMessage (
                    GH_RuntimeMessageLevel.Error,
                    "Not one of this input's choices: " + string.Join (", ", rejected)
                        + ". Expected one of: " + string.Join (", ", m_core.Choices) + ".");
            }

            ExpireSolution (false);
        }

        // The method rhino.compute invokes by reflection. See
        // TapiocaInputGuard.AssignTree for why the interface alone is not enough.
        public void AssignContextualDataTree (global::Grasshopper.DataTree<GH_String> tree)
        {
            TapiocaInputGuard.NoteIfDriven (this);

            m_fromPanel = TapiocaInputGuard.Flatten (tree);
            TapiocaInputGuard.AssignTree (this, tree);
        }

        public void ClearContextualData ()
        {
            m_fromPanel = null;
            PersistentData.Clear ();
            ExpireSolution (false);
        }

        public bool AutoAssignContextualData (GH_ParameterContext context)
        {
            return true;
        }

        // the panel's value, over a wire ------------------------------------

        /// <summary>What the panel last sent, or null.</summary>
        /// <remarks>
        /// Kept BESIDE the persistent data rather than instead of it: with no
        /// source, persistent data is what Grasshopper reads and that proven
        /// path stays untouched. This is consulted only when a source exists --
        /// see TapiocaInputGuard.ApplyOverride for why a parameter's own
        /// collection is the one place it can be applied.
        /// </remarks>
        private List<GH_String> m_fromPanel;

        protected override void CollectVolatileData_FromSources ()
        {
            base.CollectVolatileData_FromSources ();
            TapiocaInputGuard.ApplyOverride (this, m_fromPanel);
        }

        public override bool Write (GH_IWriter writer)
        {
            m_core.Write (writer);
            return base.Write (writer);
        }

        public override bool Read (GH_IReader reader)
        {
            m_core.Read (reader);
            return base.Read (reader);
        }
    }
}
