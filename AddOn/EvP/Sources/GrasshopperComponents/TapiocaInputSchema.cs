using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

using Grasshopper.Kernel;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// Reads a definition's public inputs and states them as one schema the
    /// Archicad panel can build controls from.
    /// </summary>
    /// <remarks>
    /// ⚠️ THIS IS THE SECOND DISCOVERY PATH, NOT THE ONLY ONE, AND THE TWO MUST
    /// AGREE. rhino.compute does its own scan and answers POST /io without ever
    /// calling this; the in-process worker has no such endpoint and calls this.
    /// Both walk the same objects through IGH_ContextualParameter, so the inputs
    /// they find are the same set — but the ORDER and the VALIDATION are ours to
    /// keep consistent, and this class is where that happens for the worker.
    /// Anything added here that compute cannot also report is a divergence
    /// between backends, which SPEC-RhinoCompute.md forbids.
    ///
    /// ⚠️ NO JSON LIBRARY, DELIBERATELY. The package has one PackageReference and
    /// adding Newtonsoft here would put a second copy of it beside a Rhino that
    /// already loads its own. The emitted shape is small, fixed and fully
    /// escaped below.
    /// </remarks>
    public static class TapiocaInputSchema
    {
        /// <summary>
        /// One discovered input, after ordering and validation.
        /// </summary>
        public sealed class Entry
        {
            internal Entry (ITapiocaInput input, IGH_ContextualParameter contextual, int order)
            {
                Input = input;
                Contextual = contextual;
                ResolvedOrder = order;
            }

            internal ITapiocaInput Input { get; private set; }

            /// <summary>
            /// The parameter a snapshot is written into.
            /// </summary>
            /// <remarks>
            /// ⚠️ CARRIED, NOT CAST BACK OUT OF <see cref="Input"/>. A Tapioca
            /// input IS its parameter and the cast would work; a stock "Get
            /// Number" reaches the schema through a wrapper that is not the
            /// parameter, and the same cast would quietly find nothing to write
            /// to -- a solve that succeeds with the definition's own defaults
            /// and never says the panel's values were dropped.
            /// </remarks>
            internal IGH_ContextualParameter Contextual { get; private set; }

            internal int ResolvedOrder { get; private set; }
        }

        /// <summary>
        /// Collects every Tapioca input on the document, in the order the panel
        /// should show them.
        /// </summary>
        /// <remarks>
        /// Ordering is Group, then explicit order, then canvas position.
        /// The canvas fallback is not a tiebreaker of last resort — it is what a
        /// Grasshopper author expects by default, because Grasshopper's own
        /// <c>DocumentContextuals</c> sorts by <c>Attributes.Pivot.Y</c>. An
        /// author who never touches Order still gets top-to-bottom.
        /// </remarks>
        public static IList<Entry> Discover (GH_Document document, IList<string> errors)
        {
            List<Entry> found = new List<Entry> ();

            if (document == null)
            {
                Add (errors, "No Grasshopper document is open.");
                return found;
            }

            List<IGH_DocumentObject> objects = new List<IGH_DocumentObject> ();
            foreach (IGH_DocumentObject obj in document.Objects)
            {
                objects.Add (obj);
            }

            // Canvas position decides the fallback order, so sort by it BEFORE
            // assigning positions — the index below is only meaningful once the
            // list is in canvas order.
            objects.Sort (delegate (IGH_DocumentObject a, IGH_DocumentObject b)
            {
                float ay = a.Attributes == null ? 0f : a.Attributes.Pivot.Y;
                float by = b.Attributes == null ? 0f : b.Attributes.Pivot.Y;
                return ay.CompareTo (by);
            });

            // The generic half of the contract -- stock Params > Util "Get ..."
            // parameters, which is what Grasshopper Player and rhino.compute
            // read. Keyed by the object it wraps so the ONE canvas-order loop
            // below places both kinds; a second pass appended afterwards would
            // put every stock input below every Tapioca one no matter where the
            // author put them.
            IDictionary<IGH_DocumentObject, TapiocaContextualDiscovery.ContextualInput> contextual =
                TapiocaContextualDiscovery.Inputs (document);

            for (int i = 0; i < objects.Count; i++)
            {
                IGH_Param param = objects[i] as IGH_Param;
                if (param != null && param.Locked)
                {
                    // A disabled parameter is not part of the contract. It is
                    // still on the canvas, so this is a choice the author made
                    // and not an error to report.
                    continue;
                }

                ITapiocaInput input = objects[i] as ITapiocaInput;
                IGH_ContextualParameter parameter = objects[i] as IGH_ContextualParameter;

                if (input == null)
                {
                    TapiocaContextualDiscovery.ContextualInput generic;
                    if (!contextual.TryGetValue (objects[i], out generic))
                    {
                        continue;
                    }

                    input = generic;
                    parameter = generic.Contextual;
                }

                int order = input.TapiocaOrder >= 0 ? input.TapiocaOrder : i;
                found.Add (new Entry (input, parameter, order));
            }

            found.Sort (delegate (Entry a, Entry b)
            {
                int byGroup = string.Compare (
                    a.Input.TapiocaGroup, b.Input.TapiocaGroup, StringComparison.OrdinalIgnoreCase);
                return byGroup != 0 ? byGroup : a.ResolvedOrder.CompareTo (b.ResolvedOrder);
            });

            Validate (found, errors);
            return found;
        }

        /// <summary>
        /// The rules GH-005 names, applied before a solve is ever attempted.
        /// </summary>
        /// <remarks>
        /// ⚠️ THESE ARE REFUSALS, NOT WARNINGS, AND THEY EXIST BECAUSE COMPUTE'S
        /// OWN FAILURE IS QUIET. Given two inputs with one name, rhino.compute
        /// reports a single input, lists the collision in its Errors array, and
        /// then returns a solve with EMPTY TREES — measured on BoxToMesh.gh.
        /// Nothing about that reads as "your definition is wrong" at the panel.
        /// Catching it here names the offending id while the author can still
        /// see which components collided.
        /// </remarks>
        private static void Validate (IList<Entry> entries, IList<string> errors)
        {
            Dictionary<string, int> seen = new Dictionary<string, int> (StringComparer.OrdinalIgnoreCase);

            foreach (Entry entry in entries)
            {
                string id = entry.Input.TapiocaId;

                if (string.IsNullOrEmpty (id))
                {
                    Add (errors, "An input named '" + entry.Input.TapiocaLabel
                        + "' has no id. Rename the parameter, or set its id, so Archicad can bind a control to it.");
                    continue;
                }

                if (seen.ContainsKey (id))
                {
                    seen[id] = seen[id] + 1;
                    Add (errors, "Two or more inputs share the id '" + id
                        + "'. Input ids must be unique: rhino.compute keys inputs by name, "
                        + "and a collision makes it drop every one but the first and solve with no data.");
                }
                else
                {
                    seen[id] = 1;
                }

                if (entry.Input.TapiocaTypeName == "enum" && entry.Input.TapiocaChoices.Count == 0)
                {
                    Add (errors, "The enum input '" + id + "' declares no choices, so the panel has nothing to offer.");
                }

                if (entry.Input.TapiocaMinimum.HasValue && entry.Input.TapiocaMaximum.HasValue
                    && entry.Input.TapiocaMinimum.Value > entry.Input.TapiocaMaximum.Value)
                {
                    Add (errors, "The input '" + id + "' has a minimum above its maximum.");
                }
            }
        }

        /// <summary>
        /// Emits the WorkflowSchema the Archicad panel consumes.
        /// </summary>
        public static string ToJson (
            string workflowId, string workflowName, string description, IList<Entry> entries, IList<string> errors)
        {
            StringBuilder sb = new StringBuilder ();
            sb.Append ("{\"workflowId\":").Append (Quote (workflowId));
            sb.Append (",\"name\":").Append (Quote (workflowName));
            // The definition's own words, for the palette's description band.
            // Always present, empty when the definition carries no Tapioca
            // Description component -- an absent field and an empty one would be
            // the same thing to the reader, and one of them needs no rule.
            sb.Append (",\"description\":").Append (Quote (description ?? string.Empty));
            sb.Append (",\"version\":1,\"inputs\":[");

            for (int i = 0; i < entries.Count; i++)
            {
                ITapiocaInput input = entries[i].Input;

                if (i > 0)
                {
                    sb.Append (',');
                }

                sb.Append ("{\"id\":").Append (Quote (input.TapiocaId));
                sb.Append (",\"label\":").Append (Quote (input.TapiocaLabel));
                sb.Append (",\"type\":").Append (Quote (input.TapiocaTypeName));
                sb.Append (",\"group\":").Append (Quote (input.TapiocaGroup));
                sb.Append (",\"order\":").Append (entries[i].ResolvedOrder.ToString (CultureInfo.InvariantCulture));
                sb.Append (",\"required\":").Append (input.TapiocaRequired ? "true" : "false");

                if (input.TapiocaMinimum.HasValue)
                {
                    sb.Append (",\"min\":").Append (Number (input.TapiocaMinimum.Value));
                }

                if (input.TapiocaMaximum.HasValue)
                {
                    sb.Append (",\"max\":").Append (Number (input.TapiocaMaximum.Value));
                }

                if (input.TapiocaChoices.Count > 0)
                {
                    sb.Append (",\"choices\":[");
                    for (int c = 0; c < input.TapiocaChoices.Count; c++)
                    {
                        if (c > 0)
                        {
                            sb.Append (',');
                        }

                        sb.Append (Quote (input.TapiocaChoices[c]));
                    }

                    sb.Append (']');
                }

                sb.Append (",\"default\":").Append (Quote (input.TapiocaCurrentValue));
                sb.Append ('}');
            }

            sb.Append ("],\"errors\":[");
            if (errors != null)
            {
                for (int i = 0; i < errors.Count; i++)
                {
                    if (i > 0)
                    {
                        sb.Append (',');
                    }

                    sb.Append (Quote (errors[i]));
                }
            }

            sb.Append ("]}");
            return sb.ToString ();
        }

        private static void Add (IList<string> errors, string message)
        {
            if (errors != null)
            {
                errors.Add (message);
            }
        }

        // Round-trips through double.Parse with InvariantCulture. "R" rather
        // than a fixed precision: a range the author typed must come back as
        // the number they typed.
        private static string Number (double value)
        {
            return value.ToString ("R", CultureInfo.InvariantCulture);
        }

        private static string Quote (string raw)
        {
            if (raw == null)
            {
                return "\"\"";
            }

            StringBuilder sb = new StringBuilder (raw.Length + 2);
            sb.Append ('"');

            foreach (char c in raw)
            {
                switch (c)
                {
                    case '"':
                        sb.Append ("\\\"");
                        break;
                    case '\\':
                        sb.Append ("\\\\");
                        break;
                    case '\b':
                        sb.Append ("\\b");
                        break;
                    case '\f':
                        sb.Append ("\\f");
                        break;
                    case '\n':
                        sb.Append ("\\n");
                        break;
                    case '\r':
                        sb.Append ("\\r");
                        break;
                    case '\t':
                        sb.Append ("\\t");
                        break;
                    default:
                        if (c < ' ')
                        {
                            sb.Append ("\\u").Append (((int) c).ToString ("x4", CultureInfo.InvariantCulture));
                        }
                        else
                        {
                            sb.Append (c);
                        }

                        break;
                }
            }

            sb.Append ('"');
            return sb.ToString ();
        }
    }
}
