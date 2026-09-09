using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
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
                return TapiocaInputSchema.ToJson (
                    workflowId ?? string.Empty,
                    string.IsNullOrEmpty (workflowName) ? DocumentName (document) : workflowName,
                    entries,
                    errors);
            }
            catch (Exception exception)
            {
                // A schema the worker cannot read is reported AS a schema, with
                // the reason in the errors array the panel already renders. The
                // alternative -- no answer at all -- would leave the panel
                // showing the previous definition's controls.
                return "{\"workflowId\":\"\",\"name\":\"\",\"version\":1,\"inputs\":[],\"errors\":[\""
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

                Dictionary<string, IGH_ContextualParameter> byId =
                    new Dictionary<string, IGH_ContextualParameter> (StringComparer.OrdinalIgnoreCase);
                List<string> discovery = new List<string> ();
                foreach (TapiocaInputSchema.Entry entry in TapiocaInputSchema.Discover (document, discovery))
                {
                    IGH_ContextualParameter contextual = entry.Input as IGH_ContextualParameter;
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

                    // The published interface, not compute's reflected
                    // AssignContextualDataTree: one value per input is what the
                    // panel produces, the concrete parameters convert the text
                    // themselves, and each one refuses a driven parameter with a
                    // warning the author can see.
                    target.AssignContextualData (new object[] { values[index] ?? string.Empty });
                }

                return rejected.ToString ();
            }
            catch (Exception exception)
            {
                return Describe (exception);
            }
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

                foreach (IGH_DocumentObject candidate in document.Objects)
                {
                    ITapiocaOutput output = candidate as ITapiocaOutput;
                    if (output == null)
                    {
                        continue;
                    }

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
