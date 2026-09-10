using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Text;

using Grasshopper.Kernel;
using Grasshopper.Kernel.Data;
using Grasshopper.Kernel.Types;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// The one surface <c>Tapioca.GhWorker.exe</c> binds to by reflection.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ REFLECTION IN BOTH DIRECTIONS, FOR ONE REASON, AND IT IS THE LOADER.
    /// <see cref="TapiocaBridge"/> explains the worker-ward half: this .gha may
    /// not reference the worker, because Grasshopper's loader would resolve a
    /// second copy of it. The same is true coming back. The worker cannot
    /// reference this package either — it is loaded by Grasshopper from the
    /// Libraries folder, at a path the worker does not know when it is compiled,
    /// and a compile-time reference would put a second Tapioca.Grasshopper in the
    /// process whose components are not the ones on the canvas.
    /// </para>
    /// <para>
    /// ⚠️ EVERY SIGNATURE HERE IS A BREAKING CHANGE FOR THE WORKER, AND EVERY
    /// PARAMETER IS EITHER A GRASSHOPPER TYPE OR A PRIMITIVE. Both halves get
    /// <c>GH_Document</c> from the one Grasshopper the process loaded, so it is
    /// safe in a reflected signature; a type declared in THIS assembly would not
    /// be, because the worker has no way to name it. That is why outputs come
    /// back as a flat <c>string[]</c> of quadruples rather than as a list of some
    /// tidy result class.
    /// </para>
    /// <para>
    /// ⚠️ NOTHING HERE THROWS ACROSS THE BOUNDARY. A reflected call that throws
    /// reaches the worker as a TargetInvocationException wrapping something it
    /// cannot name, on a thread that owns the whole engine. Every method below
    /// answers with a message instead.
    /// </para>
    /// </remarks>
    public static class TapiocaWorkflowFacade
    {
        /// <summary>
        /// What the last <see cref="ApplyInputs"/> did to each input, by id.
        /// </summary>
        /// <remarks>
        /// ⚠️ STATE ON A STATIC, WHICH IS ALLOWED HERE FOR EXACTLY ONE REASON:
        /// both methods run on the worker's single engine thread, one
        /// immediately after the other, on one document. It is a hand-off
        /// between two halves of one operation, not a cache -- ApplyInputs
        /// clears it and DescribeInputs reads it. The alternative was widening
        /// the reflected signature of one of them, and every signature here is
        /// a breaking change for the worker.
        /// </remarks>
        private static readonly Dictionary<string, string> s_outcome =
            new Dictionary<string, string> (StringComparer.OrdinalIgnoreCase);

        /// <summary>
        /// Writes one value into one contextual parameter, the way compute does.
        /// </summary>
        /// <remarks>
        /// <para>
        /// ⚠️ THE TREE PATH IS THE ONE THAT WORKS, AND THE INTERFACE IS THE ONE
        /// THAT DOES NOT. Measured in Archicad on BoxToMesh.gh and
        /// GetWidthMultiplication.gh: a stock Grasshopper Player "Get Number"
        /// (class GetNumberParameter) given a value through
        /// <c>IGH_ContextualParameter.AssignContextualData</c> reported
        /// afterwards, through DescribeInputs, "sources=0 volatile=0
        /// persistent=n/a holds=(nothing)" -- the value went nowhere, the solve
        /// succeeded on the definition's own defaults, and nothing said so.
        /// </para>
        /// <para>
        /// TWO reasons, and this fixes both. The parameter is not a
        /// <c>GH_PersistentParam&lt;T&gt;</c> at all (hence "persistent=n/a"),
        /// so there was no store for the interface method to fill; and it wants
        /// its own goo type, not the panel's text. compute never calls the
        /// interface either: it reflects a method literally named
        /// <c>AssignContextualDataTree</c> and hands it a
        /// <c>Grasshopper.DataTree&lt;T&gt;</c> whose T is chosen from the
        /// parameter's own TypeName -- GrasshopperDefinition.cs,
        /// BuildAndAssignContextualTree. That is the path Hops has always used,
        /// and it is why compute drives these components and we did not.
        /// </para>
        /// <para>
        /// The interface call remains as the FALLBACK, with a typed value rather
        /// than text: a parameter that declares no AssignContextualDataTree is
        /// skipped in silence by compute, and silence is the one outcome this
        /// whole trace exists to prevent.
        /// </para>
        /// </remarks>
        private static string Assign (IGH_Param param, IGH_ContextualParameter contextual, string text)
        {
            if (param == null)
            {
                contextual.AssignContextualData (new object[] { text });
                return "assigned by interface (no parameter behind it)";
            }

            // The goo type the parameter's own TypeName asks for. The same
            // switch compute makes, on the same strings.
            switch (param.TypeName)
            {
                case "Number":
                {
                    double number;
                    if (!double.TryParse (text, NumberStyles.Float, CultureInfo.InvariantCulture, out number))
                    {
                        return "REFUSED: '" + text + "' is not a number";
                    }

                    return AssignTree (param, contextual, new GH_Number (number), number);
                }

                case "Integer":
                {
                    int whole;
                    if (!int.TryParse (text, NumberStyles.Integer, CultureInfo.InvariantCulture, out whole))
                    {
                        double loose;
                        if (!double.TryParse (text, NumberStyles.Float, CultureInfo.InvariantCulture, out loose))
                        {
                            return "REFUSED: '" + text + "' is not a whole number";
                        }

                        whole = (int) Math.Round (loose);
                    }

                    return AssignTree (param, contextual, new GH_Integer (whole), whole);
                }

                case "Boolean":
                {
                    // The panel's own spelling, which FormatInputNumber and
                    // WorkflowRows both settle on: "true"/"false", lower case.
                    bool flag = string.Equals (text, "true", StringComparison.OrdinalIgnoreCase)
                        || text == "1";
                    return AssignTree (param, contextual, new GH_Boolean (flag), flag);
                }

                case "Text":
                    return AssignTree (param, contextual, new GH_String (text), text);

                default:
                    // A type this side has no goo for -- Point, Plane, Geometry.
                    // The panel already draws these rows disabled and captioned
                    // with the type; saying it again here is what makes the
                    // transcript agree with the panel.
                    return "not assigned: " + (param.TypeName ?? "unknown") + " inputs are not supported yet";
            }
        }

        /// <summary>
        /// Reflects AssignContextualDataTree, falling back to the interface.
        /// </summary>
        private static string AssignTree<T> (
            IGH_Param param, IGH_ContextualParameter contextual, T goo, object plain) where T : IGH_Goo
        {
            try
            {
                global::Grasshopper.DataTree<T> tree = new global::Grasshopper.DataTree<T> ();
                tree.Add (goo, new GH_Path (0));

                MethodInfo method = param.GetType ().GetMethod ("AssignContextualDataTree");
                if (method != null)
                {
                    method.Invoke (param, new object[] { tree });
                    return "assigned " + plain + " by data tree";
                }
            }
            catch (Exception exception)
            {
                return "REFUSED by data tree: " + Describe (exception);
            }

            try
            {
                // Typed, not text: the interface hands the value straight to a
                // converter that has no reason to accept a string for a number.
                contextual.AssignContextualData (new object[] { plain });
                return "assigned " + plain + " by interface (no data-tree method)";
            }
            catch (Exception exception)
            {
                return "REFUSED by interface: " + Describe (exception);
            }
        }

        /// <summary>
        /// Empties a parameter so an assignment REPLACES rather than adds to it.
        /// </summary>
        /// <remarks>
        /// Both stores, because a contextual parameter may use either: volatile
        /// data through <c>ClearData</c>, and persistent data through the
        /// <c>PersistentData</c> property, which is declared on the generic
        /// <c>GH_PersistentParam&lt;T&gt;</c> and so cannot be named here without
        /// knowing T. <c>IGH_Structure</c> is what the property exposes and it is
        /// enough to clear.
        ///
        /// Failure is silent on purpose: a parameter that has neither store is
        /// one this could not have corrupted, and an exception here would fail a
        /// whole snapshot over an input that needed no clearing.
        /// </remarks>
        private static void Clear (IGH_Param param)
        {
            if (param == null)
            {
                return;
            }

            try
            {
                param.ClearData ();

                System.Reflection.PropertyInfo info = param.GetType ().GetProperty ("PersistentData");
                IGH_Structure persistent = info == null ? null : info.GetValue (param, null) as IGH_Structure;
                if (persistent != null)
                {
                    persistent.Clear ();
                }
            }
            catch (Exception)
            {
            }
        }

        /// <summary>How many quadruple fields <see cref="CollectOutputs"/> emits.</summary>
        private const int OutputStride = 4;

        /// <summary>
        /// The definition's public input contract, as the WorkflowSchema JSON the
        /// Archicad panel builds controls from.
        /// </summary>
        /// <remarks>
        /// Discovery, ordering and validation are
        /// <see cref="TapiocaInputSchema"/>'s; this only names the workflow and
        /// hands the result over the boundary. Two discovery paths that disagree
        /// would be a divergence between backends, which is why there is only
        /// one of them.
        /// </remarks>
        public static string DescribeSchema (GH_Document document, string workflowId, string workflowName)
        {
            try
            {
                List<string> errors = new List<string> ();
                IList<TapiocaInputSchema.Entry> entries = TapiocaInputSchema.Discover (document, errors);
                // The description is discovered with the inputs and reported
                // through the same errors array, so an authoring mistake in it
                // (two Description components) reaches the panel the way a
                // duplicate input id does.
                string description = TapiocaDescriptionComponent.Describe (document, errors);

                return TapiocaInputSchema.ToJson (
                    workflowId ?? string.Empty,
                    string.IsNullOrEmpty (workflowName) ? DocumentName (document) : workflowName,
                    description,
                    entries,
                    errors);
            }
            catch (Exception exception)
            {
                // A schema the worker cannot read is reported AS a schema, with
                // the reason in the errors array the panel already renders. The
                // alternative -- no answer at all -- would leave the panel
                // showing the previous definition's controls.
                return "{\"workflowId\":\"\",\"name\":\"\",\"description\":\"\",\"version\":1,\"inputs\":[],\"errors\":[\""
                    + Escape (Describe (exception)) + "\"]}";
            }
        }

        /// <summary>
        /// Writes one input snapshot into the document's Tapioca inputs.
        /// </summary>
        /// <remarks>
        /// <para>
        /// Returns an empty string when every value landed, and otherwise one
        /// line per rejection. Values are applied by id; an id the definition
        /// does not declare is REPORTED rather than ignored, because the ordinary
        /// cause is a panel still showing the previous definition's controls, and
        /// silently solving with defaults would produce a plausible wrong answer.
        /// </para>
        /// <para>
        /// ⚠️ IT DOES NOT SOLVE. Assignment expires the parameters and stops;
        /// the engine thread runs exactly one solution afterwards, when the
        /// controller asks for it. An assignment that solved would make a
        /// five-input snapshot five solutions.
        /// </para>
        /// </remarks>
        public static string ApplyInputs (GH_Document document, string[] ids, string[] values)
        {
            try
            {
                if (document == null)
                {
                    return "No Grasshopper document is loaded in this session.";
                }

                if (ids == null || values == null || ids.Length != values.Length)
                {
                    return "The input snapshot's ids and values did not pair up.";
                }

                s_outcome.Clear ();

                Dictionary<string, IGH_ContextualParameter> byId =
                    new Dictionary<string, IGH_ContextualParameter> (StringComparer.OrdinalIgnoreCase);
                List<string> discovery = new List<string> ();
                foreach (TapiocaInputSchema.Entry entry in TapiocaInputSchema.Discover (document, discovery))
                {
                    IGH_ContextualParameter contextual = entry.Contextual;
                    string id = entry.Input.TapiocaId;
                    if (contextual == null || string.IsNullOrEmpty (id) || byId.ContainsKey (id))
                    {
                        // A duplicate id is already an error in the schema the
                        // panel was built from; taking the first here matches
                        // what discovery reported rather than adding a second
                        // rule about which one wins.
                        continue;
                    }

                    byId[id] = contextual;
                }

                StringBuilder rejected = new StringBuilder ();
                for (int index = 0; index < ids.Length; index++)
                {
                    string id = ids[index] ?? string.Empty;
                    IGH_ContextualParameter target;
                    if (!byId.TryGetValue (id, out target))
                    {
                        Append (rejected, "This definition has no input '" + id + "'.");
                        continue;
                    }

                    // ⚠️ EMPTIED BEFORE IT IS FILLED, AND THE MEASUREMENT IS WHY.
                    // A stock Params > Util "Get ..." parameter APPENDS what
                    // AssignContextualData hands it. Measured on BoxToMesh.gh in
                    // Archicad: a session solved four times reported 3, then 6,
                    // then 9, then 12 outputs, the extra ones being every value
                    // the panel had ever sent -- "la", then "la"+"lab", then
                    // "la"+"lab"+"labwl". The definition was not wrong and the
                    // panel was not wrong; the input had quietly become a list.
                    // Downstream that reads as a real error from a real
                    // component ("Filter component can only operate on a single
                    // Filter Index value"), which points at the definition and
                    // not at the injection that caused it. A Tapioca input never
                    // showed this because its own AssignContextualData REPLACES
                    // its persistent data; the stock ones are not ours to change.
                    IGH_Param param = target as IGH_Param;
                    Clear (param);
                    s_outcome[id] = Assign (param, target, values[index] ?? string.Empty);
                }

                return rejected.ToString ();
            }
            catch (Exception exception)
            {
                return Describe (exception);
            }
        }

        /// <summary>
        /// One line per discovered input saying what it IS and what it now
        /// HOLDS, for the panel's transcript.
        /// </summary>
        /// <remarks>
        /// <para>
        /// ⚠️ THIS IS A DIAGNOSTIC, AND IT EXISTS BECAUSE THE FAILURE IT
        /// DESCRIBES IS INVISIBLE. An input that is discovered, shown in the
        /// panel, sent a value and then contributes nothing to the solve looks
        /// exactly like an input that was never wired: the definition solves,
        /// the outputs are the defaults, and nothing anywhere says the value was
        /// dropped. There are at least four ways for that to happen and they are
        /// indistinguishable from Archicad -- the parameter has a SOURCE (so
        /// Grasshopper ignores persistent data entirely), it is LOCKED, its
        /// contextual assignment silently refused the text, or the id the panel
        /// keyed it by is not the id discovery found.
        /// </para>
        /// <para>
        /// So each line carries the parameter's own account of itself: the class
        /// that implements it, its Grasshopper type, how many sources it has,
        /// what it holds in each store, and the first value it would hand
        /// downstream. Read against the value the panel sent, that says which of
        /// the four it was -- from the panel, without a debugger, without
        /// Grasshopper's canvas open.
        /// </para>
        /// <para>
        /// Called AFTER ApplyInputs and BEFORE the solve, so "what it holds" is
        /// what the solve is about to read. Reading it after would report what
        /// the solution left behind, which is a different and less useful thing.
        /// </para>
        /// </remarks>
        public static string[] DescribeInputs (GH_Document document)
        {
            List<string> lines = new List<string> ();
            try
            {
                if (document == null)
                {
                    return lines.ToArray ();
                }

                List<string> discovery = new List<string> ();
                foreach (TapiocaInputSchema.Entry entry in TapiocaInputSchema.Discover (document, discovery))
                {
                    IGH_Param param = entry.Contextual as IGH_Param;
                    StringBuilder line = new StringBuilder ();
                    line.Append (entry.Input.TapiocaId).Append (": ");

                    if (param == null)
                    {
                        // Discovered, and not injectable. The panel is showing a
                        // control that can never do anything, which is worth one
                        // line rather than a silent no-op.
                        line.Append ("NOT a contextual parameter - nothing can be assigned to it");
                        lines.Add (line.ToString ());
                        continue;
                    }

                    line.Append (param.GetType ().Name);
                    line.Append (" type=").Append (param.TypeName ?? "?");
                    line.Append (" sources=").Append (param.SourceCount.ToString (CultureInfo.InvariantCulture));
                    if (param.SourceCount > 0)
                    {
                        // The quiet one. Grasshopper reads persistent data only
                        // on a parameter with NO source, so everything Tapioca
                        // sends this input is discarded while the solve still
                        // succeeds on the wired value.
                        line.Append (" (WIRED - injected values are ignored)");
                    }

                    if (param.Locked)
                    {
                        line.Append (" LOCKED");
                    }

                    line.Append (" volatile=")
                        .Append (param.VolatileDataCount.ToString (CultureInfo.InvariantCulture));
                    line.Append (" persistent=").Append (PersistentCount (param));
                    line.Append (" holds=").Append (Held (param));

                    // ⚠️ THE OUTCOME IS THE PART THAT CANNOT BE INFERRED FROM
                    // THE REST OF THE LINE. A Player parameter keeps an assigned
                    // value somewhere none of the counts above can see, so
                    // "volatile=0 persistent=n/a holds=(nothing)" is what a
                    // SUCCESSFUL assignment looks like on one -- and what a
                    // silent failure looks like too. Only the writer knows
                    // which, so the writer says.
                    string outcome;
                    if (s_outcome.TryGetValue (entry.Input.TapiocaId ?? string.Empty, out outcome))
                    {
                        line.Append (" <- ").Append (outcome);
                    }

                    lines.Add (line.ToString ());
                }

                foreach (string problem in discovery)
                {
                    lines.Add ("discovery: " + problem);
                }
            }
            catch (Exception exception)
            {
                lines.Add ("input trace failed: " + Describe (exception));
            }

            return lines.ToArray ();
        }

        /// <summary>How many items the parameter's persistent store holds.</summary>
        private static string PersistentCount (IGH_Param param)
        {
            try
            {
                System.Reflection.PropertyInfo info = param.GetType ().GetProperty ("PersistentData");
                IGH_Structure persistent = info == null ? null : info.GetValue (param, null) as IGH_Structure;
                if (persistent == null)
                {
                    return "n/a";
                }

                return persistent.DataCount.ToString (CultureInfo.InvariantCulture);
            }
            catch (Exception)
            {
                return "?";
            }
        }

        /// <summary>
        /// The first value the parameter would hand downstream, volatile first.
        /// </summary>
        private static string Held (IGH_Param param)
        {
            try
            {
                if (param.VolatileDataCount > 0)
                {
                    foreach (GH_Path path in param.VolatileData.Paths)
                    {
                        IList branch = param.VolatileData.get_Branch (path);
                        if (branch != null)
                        {
                            foreach (object item in branch)
                            {
                                return TextOf (item);
                            }
                        }
                    }
                }

                System.Reflection.PropertyInfo info = param.GetType ().GetProperty ("PersistentData");
                IGH_Structure persistent = info == null ? null : info.GetValue (param, null) as IGH_Structure;
                if (persistent != null && !persistent.IsEmpty)
                {
                    foreach (IGH_Goo goo in persistent.AllData (true))
                    {
                        return TextOf (goo);
                    }
                }
            }
            catch (Exception)
            {
                return "?";
            }

            return "(nothing)";
        }

        /// <summary>
        /// Reads every published output after a solution, as a flat array of
        /// {id, type, tree path, value} quadruples.
        /// </summary>
        /// <remarks>
        /// <para>
        /// One entry per ITEM, not per output: an output holding a tree of nine
        /// values produces nine quadruples that share an id and differ in their
        /// path. That is what makes an unsupported tree inspectable at the panel
        /// instead of flattened into a first value nobody can check.
        /// </para>
        /// <para>
        /// Read from <c>VolatileData</c> after the solution rather than captured
        /// during it, so that a component which solved several times in one
        /// solution contributes its final data once.
        /// </para>
        /// </remarks>
        public static string[] CollectOutputs (GH_Document document, int maxItems)
        {
            List<string> flat = new List<string> ();
            try
            {
                if (document == null)
                {
                    return flat.ToArray ();
                }

                // Both contracts, one pass. The named path first, so a
                // definition that publishes a value BOTH ways -- a Tapioca Data
                // Output inside an RH_OUT group, which is a reasonable thing to
                // do while porting one -- reports the named id ahead of the
                // group's, in the order the panel will list them.
                List<ITapiocaOutput> outputs = new List<ITapiocaOutput> ();
                foreach (IGH_DocumentObject candidate in document.Objects)
                {
                    ITapiocaOutput named = candidate as ITapiocaOutput;
                    if (named != null)
                    {
                        outputs.Add (named);
                    }
                }

                foreach (ITapiocaOutput generic in TapiocaContextualDiscovery.Outputs (document))
                {
                    outputs.Add (generic);
                }

                foreach (ITapiocaOutput output in outputs)
                {
                    IGH_Param param = output.TapiocaOutputParam;
                    if (param == null || param.VolatileData == null)
                    {
                        continue;
                    }

                    string id = string.IsNullOrEmpty (output.TapiocaOutputId)
                        ? output.TapiocaOutputLabel
                        : output.TapiocaOutputId;

                    foreach (GH_Path path in param.VolatileData.Paths)
                    {
                        IList branch = param.VolatileData.get_Branch (path);
                        if (branch == null)
                        {
                            continue;
                        }

                        foreach (object item in branch)
                        {
                            if (flat.Count / OutputStride >= maxItems)
                            {
                                // Clipped rather than refused: an output wired to
                                // a hundred thousand points is a definition
                                // choice, and dropping the OTHER outputs beside
                                // it would hide the ones that fit.
                                flat.Add (id);
                                flat.Add ("clipped");
                                flat.Add (path.ToString ());
                                flat.Add (
                                    "This output was cut off after "
                                        + maxItems.ToString (CultureInfo.InvariantCulture) + " items.");
                                return flat.ToArray ();
                            }

                            flat.Add (id);
                            flat.Add (TypeNameOf (item));
                            flat.Add (path.ToString ());
                            flat.Add (TextOf (item));
                        }
                    }
                }
            }
            catch (Exception exception)
            {
                flat.Add ("__error");
                flat.Add ("error");
                flat.Add (string.Empty);
                flat.Add (Describe (exception));
            }

            return flat.ToArray ();
        }

        /// <summary>
        /// Every runtime message on the document, as {level, component, text}
        /// triples. Level is "error", "warning" or "remark".
        /// </summary>
        public static string[] CollectDiagnostics (GH_Document document, int maxItems)
        {
            List<string> flat = new List<string> ();
            try
            {
                if (document == null)
                {
                    return flat.ToArray ();
                }

                foreach (IGH_DocumentObject candidate in document.Objects)
                {
                    IGH_ActiveObject active = candidate as IGH_ActiveObject;
                    if (active == null)
                    {
                        continue;
                    }

                    // Errors first, across the whole document, so a clipped list
                    // is clipped at the least useful end. Warnings and remarks
                    // follow in their own passes below.
                    if (!Append (flat, active, GH_RuntimeMessageLevel.Error, "error", maxItems))
                    {
                        return flat.ToArray ();
                    }
                }

                foreach (IGH_DocumentObject candidate in document.Objects)
                {
                    IGH_ActiveObject active = candidate as IGH_ActiveObject;
                    if (active != null
                        && !Append (flat, active, GH_RuntimeMessageLevel.Warning, "warning", maxItems))
                    {
                        return flat.ToArray ();
                    }
                }

                foreach (IGH_DocumentObject candidate in document.Objects)
                {
                    IGH_ActiveObject active = candidate as IGH_ActiveObject;
                    if (active != null
                        && !Append (flat, active, GH_RuntimeMessageLevel.Remark, "remark", maxItems))
                    {
                        return flat.ToArray ();
                    }
                }
            }
            catch (Exception exception)
            {
                flat.Add ("error");
                flat.Add ("Tapioca");
                flat.Add (Describe (exception));
            }

            return flat.ToArray ();
        }

        /// <summary>
        /// The packages this document's components actually came from, as
        /// "name version" lines.
        /// </summary>
        /// <remarks>
        /// Recorded with every solution revision (HANDOFF-GHHost.md §12): a
        /// stored revision that cannot say which packages produced it cannot be
        /// told apart from one produced by a different set of them after an
        /// upgrade.
        /// </remarks>
        public static string[] DescribeDependencies (GH_Document document)
        {
            List<string> lines = new List<string> ();
            try
            {
                if (document == null || !global::Grasshopper.Instances.IsComponentServer
                    || global::Grasshopper.Instances.ComponentServer == null)
                {
                    return lines.ToArray ();
                }

                HashSet<Guid> seen = new HashSet<Guid> ();
                SortedDictionary<string, string> libraries = new SortedDictionary<string, string> (
                    StringComparer.OrdinalIgnoreCase);

                foreach (IGH_DocumentObject candidate in document.Objects)
                {
                    if (candidate == null || !seen.Add (candidate.ComponentGuid))
                    {
                        continue;
                    }

                    // FindAssemblyByObject, the call compute uses for its own
                    // installed-plugins endpoint. It maps a COMPONENT guid to
                    // the library that emitted it, which is the question being
                    // asked here; resolving a proxy first would answer it in two
                    // steps and fail differently at each of them.
                    GH_AssemblyInfo library = global::Grasshopper.Instances.ComponentServer.FindAssemblyByObject (
                        candidate.ComponentGuid);
                    if (library == null || string.IsNullOrEmpty (library.Name))
                    {
                        continue;
                    }

                    // Grasshopper's own components are not a dependency worth
                    // fingerprinting: they ship with the Rhino this revision
                    // already records, and listing them would bury the packages
                    // that actually vary between machines.
                    if (library.IsCoreLibrary)
                    {
                        continue;
                    }

                    libraries[library.Name] = VersionOf (library);
                }

                foreach (KeyValuePair<string, string> entry in libraries)
                {
                    lines.Add (entry.Key + " " + entry.Value);
                }
            }
            catch (Exception exception)
            {
                lines.Add ("(dependencies could not be read: " + Describe (exception) + ")");
            }

            return lines.ToArray ();
        }

        /// <summary>
        /// A library's declared version, or its assembly's when it declares
        /// none. Mirrors what compute reports for the same question.
        /// </summary>
        private static string VersionOf (GH_AssemblyInfo library)
        {
            if (!string.IsNullOrEmpty (library.Version))
            {
                return library.Version;
            }

            try
            {
                return library.Assembly == null
                    ? "(no version)"
                    : library.Assembly.GetName ().Version.ToString ();
            }
            catch (Exception)
            {
                return "(no version)";
            }
        }

        private static bool Append (
            List<string> flat, IGH_ActiveObject active, GH_RuntimeMessageLevel level, string levelName, int maxItems)
        {
            foreach (string message in active.RuntimeMessages (level))
            {
                if (flat.Count / 3 >= maxItems)
                {
                    return false;
                }

                flat.Add (levelName);
                // Nickname AND guid: a bare nickname in a definition with three
                // components called "Number" is not actionable, and the guid is
                // what makes it so in a diagnostics block.
                flat.Add ((active.NickName ?? string.Empty) + " {" + active.InstanceGuid + "}");
                flat.Add (message ?? string.Empty);
            }

            return true;
        }

        private static string DocumentName (GH_Document document)
        {
            if (document == null)
            {
                return string.Empty;
            }

            return string.IsNullOrWhiteSpace (document.DisplayName) ? "(unsaved definition)" : document.DisplayName;
        }

        /// <summary>
        /// The NEUTRAL type name, never a RhinoCommon one.
        /// </summary>
        /// <remarks>
        /// The panel and the stored revision are Tapioca's, and naming a
        /// GH_Number as "GH_Number" there would leak an implementation type into
        /// a contract that has to outlive it. Anything without a neutral name is
        /// "value", and its text form is what makes it inspectable.
        /// </remarks>
        private static string TypeNameOf (object item)
        {
            if (item == null)
            {
                return "null";
            }

            if (item is GH_Number || item is GH_Integer)
            {
                return item is GH_Integer ? "integer" : "number";
            }

            if (item is GH_Boolean)
            {
                return "boolean";
            }

            if (item is GH_String)
            {
                return "text";
            }

            if (item is GH_Point)
            {
                return "point";
            }

            if (item is GH_Vector)
            {
                return "vector";
            }

            if (item is GH_Plane)
            {
                return "plane";
            }

            return "value";
        }

        private static string TextOf (object item)
        {
            if (item == null)
            {
                return string.Empty;
            }

            IGH_Goo goo = item as IGH_Goo;
            if (goo == null)
            {
                return Convert.ToString (item, CultureInfo.InvariantCulture) ?? string.Empty;
            }

            // ⚠️ ScriptVariable BEFORE ToString, AND InvariantCulture AFTER IT.
            // A GH_Number's ToString is localised -- it renders 0.5 as "0,5" on a
            // machine whose list separator is a comma -- and a panel that parses
            // what it is sent would read that as two values or as five. The
            // unwrapped script variable is a plain double, which formats the same
            // everywhere.
            try
            {
                object raw = goo.ScriptVariable ();
                if (raw is double || raw is float || raw is int || raw is long || raw is decimal)
                {
                    return Convert.ToString (raw, CultureInfo.InvariantCulture) ?? string.Empty;
                }

                if (raw is bool)
                {
                    return ((bool) raw) ? "true" : "false";
                }
            }
            catch (Exception)
            {
                // A goo whose ScriptVariable throws still has a ToString, and
                // one readable line is worth more here than a lost output.
            }

            return goo.ToString () ?? string.Empty;
        }

        private static void Append (StringBuilder into, string line)
        {
            if (into.Length > 0)
            {
                into.Append (Environment.NewLine);
            }

            into.Append (line);
        }

        private static string Describe (Exception exception)
        {
            return exception.GetType ().Name + ": " + exception.Message;
        }

        private static string Escape (string raw)
        {
            if (string.IsNullOrEmpty (raw))
            {
                return string.Empty;
            }

            return raw.Replace ("\\", "\\\\").Replace ("\"", "\\\"").Replace ("\r", " ").Replace ("\n", " ");
        }
    }
}
