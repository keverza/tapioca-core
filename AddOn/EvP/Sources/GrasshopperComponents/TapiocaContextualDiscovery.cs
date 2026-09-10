using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;

using Grasshopper.Kernel;
using Grasshopper.Kernel.Data;
using Grasshopper.Kernel.Special;
using Grasshopper.Kernel.Types;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// Finds the inputs and outputs a definition declares WITHOUT any Tapioca
    /// component: the Grasshopper Player contract.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ THE TWO CONTRACTS ARE FOR DIFFERENT THINGS, AND THIS ONE IS THE
    /// GENERIC HALF. A Tapioca input parameter carries Archicad meaning — an
    /// element, a selection, an attribute — and is ours to define. A definition
    /// that only wants a number, a point or a piece of text has already had a
    /// way to say so for years: the stock <c>Params &gt; Util</c> "Get ..."
    /// components, which are what Grasshopper Player and rhino.compute read.
    /// Making an author replace those with Tapioca copies would fork every
    /// existing definition for nothing. So both are discovered, and a Tapioca
    /// input WINS on an id collision because it is the one carrying the richer
    /// contract.
    /// </para>
    /// <para>
    /// ⚠️ EVERY RULE BELOW IS COMPUTE'S, DELIBERATELY, AND THE CITATION IS THE
    /// POINT. compute.rhino3d-9.x/src/compute.geometry/GrasshopperDefinition.cs
    /// decides what an input IS (an <c>IGH_ContextualParameter</c> that is not
    /// Locked, named by its NickName — Construct, ~line 216), where a bound
    /// comes from (a non-public <c>Minimum</c>/<c>Maximum</c> on the contextual
    /// parameter, else a single wired <c>GH_NumberSlider</c> source —
    /// InputGroup.GetMinimum, ~line 955), and what an output is (the first input
    /// of a <c>ContextBakeComponent</c> or <c>ContextPrintComponent</c>, and any
    /// group nicknamed RH_OUT — ~line 226 and ~line 264). SPEC-RhinoCompute.md
    /// forbids a divergence between backends; the only way to keep that promise
    /// is to take the rules from the other backend rather than invent parallel
    /// ones here.
    /// </para>
    /// <para>
    /// ⚠️ THE PLAYER COMPONENTS CANNOT BE REFERENCED, ONLY RECOGNISED. They live
    /// in Rhino's own Grasshopper Player plug-in, which is not a package this
    /// project may depend on, so they are matched BY CLASS NAME exactly as
    /// compute matches them. A rename on Rhino's side turns them back into
    /// ordinary components — visible on the canvas, absent from the schema —
    /// rather than into a load failure.
    /// </para>
    /// </remarks>
    internal static class TapiocaContextualDiscovery
    {
        /// <summary>Grasshopper's own type names, as compute switches on them.</summary>
        /// <remarks>
        /// A name this does not know is passed through UNTRANSLATED rather than
        /// dropped: the Archicad panel renders an unknown type as a disabled row
        /// captioned with the type it asks for, which tells the author that
        /// their Get Point input was seen and cannot yet be offered. A dropped
        /// input would read as a definition with fewer inputs.
        /// </remarks>
        private static string SchemaTypeName (IGH_Param param)
        {
            switch (param.TypeName)
            {
                case "Number":
                    return "number";
                case "Integer":
                    return "integer";
                case "Boolean":
                    return "boolean";
                case "Text":
                    return "string";
                default:
                    return param.TypeName ?? "unknown";
            }
        }

        /// <summary>
        /// Every generic input on the document, in no particular order.
        /// </summary>
        /// <remarks>
        /// Ordering is the caller's: <see cref="TapiocaInputSchema.Discover"/>
        /// sorts by canvas position and group, and doing it twice in two places
        /// is how the two paths would come to disagree.
        /// </remarks>
        internal static IDictionary<IGH_DocumentObject, ContextualInput> Inputs (GH_Document document)
        {
            Dictionary<IGH_DocumentObject, ContextualInput> found =
                new Dictionary<IGH_DocumentObject, ContextualInput> ();
            if (document == null)
            {
                return found;
            }

            Dictionary<IGH_Param, string> groupNames = GroupNames (document, "RH_IN");

            foreach (IGH_DocumentObject obj in document.Objects)
            {
                // A Tapioca input is discovered by the other path, with its own
                // id, label, group and bounds. Wrapping it here as well would
                // put every one of them in the schema twice.
                if (obj is ITapiocaInput)
                {
                    continue;
                }

                IGH_ContextualParameter contextual = obj as IGH_ContextualParameter;
                IGH_Param param = obj as IGH_Param;
                if (contextual == null || param == null || param.Locked)
                {
                    continue;
                }

                string name;
                if (!groupNames.TryGetValue (param, out name))
                {
                    name = param.NickName;
                }

                found[obj] = new ContextualInput (param, contextual, name);
            }

            return found;
        }

        /// <summary>
        /// Every generic output on the document: Context Bake, Context Print,
        /// and the first object of each RH_OUT group.
        /// </summary>
        /// <remarks>
        /// ⚠️ CONTEXT PRINT IS THE ONE THAT ANSWERS "WHERE DID MY OUTPUT GO".
        /// A definition written for the Player publishes text through Context
        /// Print and geometry through Context Bake; neither is a Tapioca
        /// component, so before this existed such a definition solved correctly
        /// and reported nothing at all. Its printed lines are ordinary output
        /// values here, which is what puts them in the panel's log.
        /// </remarks>
        internal static IList<ITapiocaOutput> Outputs (GH_Document document)
        {
            List<ITapiocaOutput> found = new List<ITapiocaOutput> ();
            if (document == null)
            {
                return found;
            }

            foreach (IGH_DocumentObject obj in document.Objects)
            {
                if (obj is ITapiocaOutput)
                {
                    continue; // the named path owns it
                }

                string className = obj.GetType ().Name;
                if (className != "ContextBakeComponent" && className != "ContextPrintComponent")
                {
                    continue;
                }

                GH_Component component = obj as GH_Component;
                if (component == null || component.Locked || component.Params.Input.Count == 0)
                {
                    continue;
                }

                IGH_Param param = component.Params.Input[0];
                found.Add (new ContextualOutput (param, param.NickName));
            }

            foreach (KeyValuePair<IGH_Param, string> pair in GroupNames (document, "RH_OUT"))
            {
                if (pair.Key.Locked)
                {
                    continue;
                }

                found.Add (new ContextualOutput (pair.Key, pair.Value));
            }

            return found;
        }

        /// <summary>
        /// The parameters an RH_IN / RH_OUT group renames, and what to call them.
        /// </summary>
        /// <remarks>
        /// ⚠️ THE PREFIX IS KEPT IN THE NAME, WHICH LOOKS WRONG AND IS NOT.
        /// compute reports the WHOLE group nickname as the input's name — Hops
        /// sends "RH_IN:Radius" back verbatim — so stripping it here would give
        /// the same definition two different ids depending on which backend read
        /// it, and an input applied by id would silently miss. The panel is told
        /// a friendlier LABEL instead; only the label is ours to choose.
        ///
        /// A group holding a component rather than a parameter is skipped: the
        /// output side of compute's rule spreads one group over several outputs,
        /// which has no meaning for an input and no caller here.
        /// </remarks>
        private static Dictionary<IGH_Param, string> GroupNames (GH_Document document, string marker)
        {
            Dictionary<IGH_Param, string> names = new Dictionary<IGH_Param, string> ();

            foreach (IGH_DocumentObject obj in document.Objects)
            {
                GH_Group group = obj as GH_Group;
                if (group == null)
                {
                    continue;
                }

                string nickname = group.NickName ?? string.Empty;
                if (nickname.IndexOf (marker, StringComparison.Ordinal) < 0)
                {
                    continue;
                }

                IList<IGH_DocumentObject> members = group.Objects ();
                if (members == null || members.Count == 0)
                {
                    continue;
                }

                IGH_Param param = members[0] as IGH_Param;
                if (param != null && !names.ContainsKey (param))
                {
                    names[param] = nickname;
                }
            }

            return names;
        }

        /// <summary>The part of an RH_IN:Radius name a person should read.</summary>
        internal static string FriendlyLabel (string name)
        {
            if (string.IsNullOrEmpty (name))
            {
                return string.Empty;
            }

            int colon = name.IndexOf (':');
            if (colon >= 0 && colon + 1 < name.Length)
            {
                return name.Substring (colon + 1).Trim ();
            }

            return name.Trim ();
        }

        /// <summary>
        /// One stock contextual parameter, stated as the schema states inputs.
        /// </summary>
        /// <remarks>
        /// Read ONCE at construction. The alternative — properties that reach
        /// back into the document each time they are asked — would make the
        /// schema depend on when each field happened to be read, and discovery
        /// runs while the engine thread owns the document.
        /// </remarks>
        internal sealed class ContextualInput : ITapiocaInput
        {
            private readonly IGH_ContextualParameter m_contextual;
            private readonly string m_id;
            private readonly string m_label;
            private readonly string m_type;
            private readonly string m_value;
            private readonly bool m_required;
            private readonly double? m_minimum;
            private readonly double? m_maximum;

            internal ContextualInput (IGH_Param param, IGH_ContextualParameter contextual, string name)
            {
                m_contextual = contextual;
                m_id = (name ?? string.Empty).Trim ();
                m_label = FriendlyLabel (m_id);
                m_type = SchemaTypeName (param);
                m_value = CurrentValue (param);
                m_required = contextual.AtLeast > 0;
                m_minimum = Bound (param, contextual, "Minimum");
                m_maximum = Bound (param, contextual, "Maximum");
            }

            /// <summary>
            /// The parameter this wrapper speaks for.
            /// </summary>
            /// <remarks>
            /// ⚠️ THE WRAPPER IS NOT THE PARAMETER, AND ASSIGNMENT NEEDS THE
            /// PARAMETER. Applying a snapshot casts a discovered input to
            /// IGH_ContextualParameter; on a Tapioca input that cast succeeds
            /// because the parameter IS the input, and on one of these it would
            /// silently find nothing to write to. Discovery carries this across
            /// instead, so both kinds are applied through one code path.
            /// </remarks>
            internal IGH_ContextualParameter Contextual { get { return m_contextual; } }

            public string TapiocaTypeName { get { return m_type; } }

            public string TapiocaId { get { return m_id; } }

            public string TapiocaLabel { get { return m_label.Length > 0 ? m_label : m_id; } }

            /// <summary>
            /// Always empty, so these sort by canvas position among themselves.
            /// </summary>
            /// <remarks>
            /// A group of their own ("Grasshopper inputs", say) was the tempting
            /// alternative and would have been a lie about the definition: the
            /// author grouped nothing, and a heading they never wrote appearing
            /// above their inputs is the panel inventing structure.
            /// </remarks>
            public string TapiocaGroup { get { return string.Empty; } }

            /// <summary>Unset, so discovery orders by canvas position.</summary>
            public int TapiocaOrder { get { return -1; } }

            public bool TapiocaRequired { get { return m_required; } }

            public double? TapiocaMinimum { get { return m_minimum; } }

            public double? TapiocaMaximum { get { return m_maximum; } }

            /// <summary>Always empty: a stock parameter declares no choices.</summary>
            public IList<string> TapiocaChoices { get { return new List<string> (); } }

            public string TapiocaCurrentValue { get { return m_value; } }
        }

        /// <summary>One Context Bake / Context Print / RH_OUT parameter.</summary>
        private sealed class ContextualOutput : ITapiocaOutput
        {
            private readonly IGH_Param m_param;
            private readonly string m_id;

            internal ContextualOutput (IGH_Param param, string name)
            {
                m_param = param;
                m_id = string.IsNullOrEmpty (name) ? param.Name : name;
            }

            /// <summary>
            /// The name compute would report, prefix and all.
            /// </summary>
            /// <remarks>
            /// A Context Print's value is NOT marked as printed on the way out,
            /// and the temptation to mark it was worth resisting: the id is the
            /// only field that survives to the panel, compute reports the bare
            /// nickname there, and a "print:" this side would give one
            /// definition two different output ids depending on which backend
            /// read it. The printed text arrives as an ordinary named value,
            /// which is all the panel's transcript needs.
            /// </remarks>
            public string TapiocaOutputId { get { return m_id; } }

            public string TapiocaOutputLabel { get { return FriendlyLabel (m_id); } }

            public IGH_Param TapiocaOutputParam { get { return m_param; } }
        }

        /// <summary>
        /// A bound, by compute's own rule and in compute's own order.
        /// </summary>
        /// <remarks>
        /// ⚠️ REFLECTION OVER A NON-PUBLIC PROPERTY, WHICH IS NOT A SHORTCUT.
        /// The stock "Get Number" parameter keeps its domain in a property the
        /// interface does not expose; compute reads it exactly this way, and
        /// asking a different question here would produce a different domain
        /// hint on the two backends for one definition. A saturated bound
        /// (double.MinValue / int.MaxValue) means "unbounded" and is reported as
        /// no bound at all, again as compute does.
        /// </remarks>
        private static double? Bound (IGH_Param param, IGH_ContextualParameter contextual, string propertyName)
        {
            try
            {
                PropertyInfo info = contextual.GetType ().GetProperty (
                    propertyName, BindingFlags.NonPublic | BindingFlags.Instance);
                if (info != null)
                {
                    object raw = info.GetValue (contextual, null);
                    if (raw != null)
                    {
                        double value = Convert.ToDouble (raw, CultureInfo.InvariantCulture);
                        if (!Saturated (value))
                        {
                            return value;
                        }
                    }
                }

                // A parameter with exactly one wired source takes the source's
                // domain. That source is also what makes injection USELESS --
                // Grasshopper reads persistent data only on a parameter with no
                // source -- but saying so is TapiocaInputGuard's job, and a
                // silent refusal here would leave the row with no domain and no
                // explanation.
                if (param.SourceCount == 1)
                {
                    GH_NumberSlider slider = param.Sources[0] as GH_NumberSlider;
                    if (slider != null)
                    {
                        return propertyName == "Minimum"
                            ? (double) slider.Slider.Minimum
                            : (double) slider.Slider.Maximum;
                    }
                }
            }
            catch (Exception)
            {
                // A bound is a hint. Losing one costs a domain caption; letting
                // the exception out would cost the whole schema.
            }

            return null;
        }

        private static bool Saturated (double value)
        {
            return value <= double.MinValue + 1.0 || value >= double.MaxValue - 1.0
                || value <= int.MinValue + 1.0 || value >= int.MaxValue - 1.0;
        }

        /// <summary>
        /// What the parameter holds now, as the schema spells a default.
        /// </summary>
        /// <remarks>
        /// ⚠️ IT DOES NOT COLLECT DATA, AND COMPUTE DOES. compute's InputGroup
        /// constructor calls ClearData/CollectData to capture a default, which
        /// EXPIRES the parameter as a side effect of being asked what it holds.
        /// This runs on a live session's document that a solve may already have
        /// filled, so it reads what is there — volatile data first, persistent
        /// data as the fallback — and never touches the solution state.
        /// </remarks>
        private static string CurrentValue (IGH_Param param)
        {
            try
            {
                if (param.VolatileDataCount > 0)
                {
                    foreach (GH_Path path in param.VolatileData.Paths)
                    {
                        IList branch = param.VolatileData.get_Branch (path);
                        if (branch == null)
                        {
                            continue;
                        }

                        foreach (object item in branch)
                        {
                            if (item != null)
                            {
                                return Text (item);
                            }
                        }
                    }
                }

                // GH_PersistentParam<T> is generic, so its PersistentData cannot
                // be named here without knowing T. The interface it exposes is
                // IGH_Structure, which is enough to read the first item.
                PropertyInfo info = param.GetType ().GetProperty ("PersistentData");
                IGH_Structure persistent = info == null ? null : info.GetValue (param, null) as IGH_Structure;
                if (persistent != null && !persistent.IsEmpty)
                {
                    foreach (IGH_Goo goo in persistent.AllData (true))
                    {
                        if (goo != null)
                        {
                            return Text (goo);
                        }
                    }
                }
            }
            catch (Exception)
            {
                // Same trade as a bound: a missing default is an empty field.
            }

            return string.Empty;
        }

        private static string Text (object item)
        {
            IGH_Goo goo = item as IGH_Goo;
            if (goo == null)
            {
                return item == null ? string.Empty : item.ToString ();
            }

            // ScriptVariable rather than ToString: a GH_Number's ToString is
            // its formatted display and a GH_Boolean's is "True", neither of
            // which round-trips through the panel's parser.
            object value = null;
            try
            {
                value = goo.ScriptVariable ();
            }
            catch (Exception)
            {
                value = null;
            }

            if (value is double)
            {
                return ((double) value).ToString ("R", CultureInfo.InvariantCulture);
            }

            if (value is int)
            {
                return ((int) value).ToString (CultureInfo.InvariantCulture);
            }

            if (value is bool)
            {
                return ((bool) value) ? "true" : "false";
            }

            return value == null ? goo.ToString () : value.ToString ();
        }
    }
}
