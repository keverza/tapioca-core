using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

using GH_IO.Serialization;
using Grasshopper.Kernel;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// The metadata every Tapioca input parameter carries, and the rules that
    /// keep it meaning the same thing on both backends.
    /// </summary>
    /// <remarks>
    /// ⚠️ THIS IS STATE ON A PARAMETER, NOT A COMPONENT, AND THAT IS FORCED.
    /// Input injection happens through <c>IGH_ContextualParameter</c>, which
    /// Grasshopper only ever looks for on document objects it can hand data to;
    /// both backends find it the same way. rhino.compute's
    /// <c>compute.geometry.dll</c> references that interface directly, and
    /// Grasshopper's own <c>GH_RhinoScriptInterface.DocumentContextuals</c>
    /// collects it for the in-process path. One implementation, two backends —
    /// see docs/architecture/api/SPEC-RhinoCompute.md.
    ///
    /// ⚠️ C# HAS NO MULTIPLE INHERITANCE AND THE FIVE INPUT TYPES MUST EACH
    /// DERIVE FROM A DIFFERENT <c>Param_*</c>. So the shared behaviour lives
    /// here as a field each parameter owns and forwards to, rather than in a
    /// base class that cannot exist. Five near-identical copies of the identity
    /// and persistence rules is exactly the drift this class prevents.
    ///
    /// ⚠️ IDENTITY IS THE CANVAS NICKNAME, BECAUSE COMPUTE SAYS SO — see
    /// <see cref="EffectiveId"/>, which carries the measurement. Duplicates are
    /// a hard failure and a QUIET one: three stock Hops "Get Number" components
    /// with unset nicknames made /io report ONE input and return "Multiple input
    /// parameters with the same name were detected." twice, after which the
    /// solve returned empty trees and said nothing more. A Tapioca input adds a
    /// persisted <see cref="Id"/> as the fallback when the author never renamed
    /// anything, so an input always has SOME stable identity.
    /// </remarks>
    internal sealed class TapiocaInputCore
    {
        // The wire/schema type name. Fixed per parameter class and never
        // inferred from the GH type, because the schema is a contract with the
        // Archicad panel and Param_Number is also what an Integer input would
        // report if it were asked.
        private readonly string m_typeName;

        private string m_id = string.Empty;
        private string m_label = string.Empty;
        private string m_group = string.Empty;
        private string m_description = string.Empty;
        private int m_order = -1;
        private bool m_required = true;

        // Number/Integer only. Null means unbounded; the panel then builds a
        // plain edit rather than a spinner with stops.
        private double? m_minimum;
        private double? m_maximum;

        // Enum only. The ORDER of this list is the schema's order and the popup's
        // row order, so it is a list and never a set.
        private readonly List<string> m_choices = new List<string>();

        internal TapiocaInputCore (string typeName)
        {
            m_typeName = typeName;
        }

        internal string TypeName
        {
            get { return m_typeName; }
        }

        /// <summary>
        /// The stable identity the Archicad panel keys its control by, and the
        /// name compute injects against.
        /// </summary>
        /// <remarks>
        /// Empty until the author sets one or <see cref="EnsureId"/> generates
        /// it. It is NOT auto-generated on construction: a generated id that
        /// later gets persisted would make two copy-pasted inputs collide in a
        /// way the author never chose and cannot see.
        /// </remarks>
        internal string Id
        {
            get { return m_id; }
            set { m_id = Sanitize (value); }
        }

        internal string Label
        {
            get { return m_label; }
            set { m_label = value == null ? string.Empty : value.Trim (); }
        }

        internal string Group
        {
            get { return m_group; }
            set { m_group = value == null ? string.Empty : value.Trim (); }
        }

        internal string Description
        {
            get { return m_description; }
            set { m_description = value == null ? string.Empty : value.Trim (); }
        }

        /// <summary>
        /// Explicit ordering within a group. Negative means "unset".
        /// </summary>
        /// <remarks>
        /// ⚠️ UNSET IS NOT ZERO, AND THE FALLBACK IS THE CANVAS. Grasshopper's
        /// own <c>DocumentContextuals</c> sorts contextual parameters by
        /// <c>Attributes.Pivot.Y</c> — vertical position on the canvas — and a
        /// Player author already expects that. Collapsing "unset" to 0 would
        /// silently reorder every input against the canvas the author arranged.
        /// The discovery pass fills this from pivot Y when it is negative.
        /// </remarks>
        internal int Order
        {
            get { return m_order; }
            set { m_order = value; }
        }

        internal bool Required
        {
            get { return m_required; }
            set { m_required = value; }
        }

        internal double? Minimum
        {
            get { return m_minimum; }
            set { m_minimum = value; }
        }

        internal double? Maximum
        {
            get { return m_maximum; }
            set { m_maximum = value; }
        }

        internal List<string> Choices
        {
            get { return m_choices; }
        }

        /// <summary>
        /// The identity both backends must agree on, and the caption built from it.
        /// </summary>
        /// <remarks>
        /// ⚠️ COMPUTE KEYS INPUTS BY NICKNAME, NOT BY NAME. Measured: a Tapioca
        /// Number renamed to "HeightNum" on the canvas came back from POST /io
        /// as Name "HeightNum" with Nickname null, while the parameter's own
        /// Name still read "Tapioca_Number". Two identities for one input is a
        /// panel that binds a control to an id compute will never inject into,
        /// and the failure is silent — the solve succeeds with no data, which is
        /// exactly what an empty output tree looks like.
        ///
        /// So the canvas nickname WINS when the author set one, and the stored
        /// id is the fallback. Renaming on the canvas is how a Grasshopper
        /// author expects to name a Player input, and this makes that the same
        /// act as setting the id.
        /// </remarks>
        internal string EffectiveId (string nickName)
        {
            string fromNick = Sanitize (nickName);
            return fromNick.Length > 0 ? fromNick : m_id;
        }

        internal string EffectiveLabel (string nickName)
        {
            return m_label.Length > 0 ? m_label : EffectiveId (nickName);
        }

        /// <summary>
        /// Derives an id from the author's label when none was set, so a
        /// definition still solves rather than failing discovery outright.
        /// </summary>
        /// <remarks>
        /// GH-005 calls for a "deterministic generated ID" on a missing one, and
        /// deterministic is the load-bearing word: the id must not change
        /// between two discovery passes over the same unedited definition, or a
        /// panel value the user typed would rebind to a different control. So it
        /// derives from the label, and falls back to the instance GUID — which
        /// Grasshopper persists — rather than to a counter, which would depend
        /// on document traversal order.
        /// </remarks>
        internal void EnsureId (Guid instanceGuid)
        {
            if (m_id.Length > 0)
            {
                return;
            }

            string fromLabel = Sanitize (m_label);
            m_id = fromLabel.Length > 0
                ? fromLabel
                : "input_" + instanceGuid.ToString ("N").Substring (0, 8);
        }

        /// <summary>
        /// Ids are matched by the panel and by compute, so they may not carry
        /// whitespace or the quoting hazards a JSON schema would have to escape.
        /// </summary>
        internal static string Sanitize (string raw)
        {
            if (string.IsNullOrEmpty (raw))
            {
                return string.Empty;
            }

            StringBuilder sb = new StringBuilder (raw.Length);
            foreach (char c in raw.Trim ())
            {
                if (char.IsLetterOrDigit (c) || c == '_' || c == '.' || c == '-')
                {
                    sb.Append (c);
                }
                else if (c == ' ')
                {
                    sb.Append ('_');
                }
            }

            return sb.ToString ();
        }

        // ── persistence ──────────────────────────────────────────────────────
        //
        // ⚠️ EVERY FIELD READ BACK MUST BE GUARDED. A definition saved before a
        // field existed is a normal thing to open, and GH_IReader throws rather
        // than returning a default for a chunk item that is not there.

        private const string KeyId = "TapiocaInputId";
        private const string KeyLabel = "TapiocaInputLabel";
        private const string KeyGroup = "TapiocaInputGroup";
        private const string KeyDescription = "TapiocaInputDescription";
        private const string KeyOrder = "TapiocaInputOrder";
        private const string KeyRequired = "TapiocaInputRequired";
        private const string KeyHasMinimum = "TapiocaInputHasMinimum";
        private const string KeyMinimum = "TapiocaInputMinimum";
        private const string KeyHasMaximum = "TapiocaInputHasMaximum";
        private const string KeyMaximum = "TapiocaInputMaximum";
        private const string KeyChoiceCount = "TapiocaInputChoiceCount";
        private const string KeyChoice = "TapiocaInputChoice";

        internal void Write (GH_IWriter writer)
        {
            writer.SetString (KeyId, m_id);
            writer.SetString (KeyLabel, m_label);
            writer.SetString (KeyGroup, m_group);
            writer.SetString (KeyDescription, m_description);
            writer.SetInt32 (KeyOrder, m_order);
            writer.SetBoolean (KeyRequired, m_required);

            writer.SetBoolean (KeyHasMinimum, m_minimum.HasValue);
            if (m_minimum.HasValue)
            {
                writer.SetDouble (KeyMinimum, m_minimum.Value);
            }

            writer.SetBoolean (KeyHasMaximum, m_maximum.HasValue);
            if (m_maximum.HasValue)
            {
                writer.SetDouble (KeyMaximum, m_maximum.Value);
            }

            writer.SetInt32 (KeyChoiceCount, m_choices.Count);
            for (int i = 0; i < m_choices.Count; i++)
            {
                writer.SetString (KeyChoice, i, m_choices[i]);
            }
        }

        internal void Read (GH_IReader reader)
        {
            if (reader.ItemExists (KeyId))
            {
                m_id = reader.GetString (KeyId);
            }

            if (reader.ItemExists (KeyLabel))
            {
                m_label = reader.GetString (KeyLabel);
            }

            if (reader.ItemExists (KeyGroup))
            {
                m_group = reader.GetString (KeyGroup);
            }

            if (reader.ItemExists (KeyDescription))
            {
                m_description = reader.GetString (KeyDescription);
            }

            if (reader.ItemExists (KeyOrder))
            {
                m_order = reader.GetInt32 (KeyOrder);
            }

            if (reader.ItemExists (KeyRequired))
            {
                m_required = reader.GetBoolean (KeyRequired);
            }

            m_minimum = null;
            if (reader.ItemExists (KeyHasMinimum) && reader.GetBoolean (KeyHasMinimum)
                && reader.ItemExists (KeyMinimum))
            {
                m_minimum = reader.GetDouble (KeyMinimum);
            }

            m_maximum = null;
            if (reader.ItemExists (KeyHasMaximum) && reader.GetBoolean (KeyHasMaximum)
                && reader.ItemExists (KeyMaximum))
            {
                m_maximum = reader.GetDouble (KeyMaximum);
            }

            m_choices.Clear ();
            if (reader.ItemExists (KeyChoiceCount))
            {
                int count = reader.GetInt32 (KeyChoiceCount);
                for (int i = 0; i < count; i++)
                {
                    if (reader.ItemExists (KeyChoice, i))
                    {
                        m_choices.Add (reader.GetString (KeyChoice, i));
                    }
                }
            }
        }

        // ── value coercion ───────────────────────────────────────────────────
        //
        // ⚠️ WHAT ARRIVES IS NOT WHAT THE PANEL SENT. AssignContextualData takes
        // a bare IEnumerable, and the two backends fill it differently: compute
        // deserializes JSON into CLR primitives and strings, while the in-process
        // path may hand over GH_ types straight from another parameter. Neither
        // is wrong, so every coercion below accepts both and refuses anything
        // else rather than guessing.

        internal static bool TryToDouble (object raw, out double value)
        {
            value = 0.0;

            if (raw == null)
            {
                return false;
            }

            if (raw is double d)
            {
                value = d;
                return true;
            }

            if (raw is int i)
            {
                value = i;
                return true;
            }

            if (raw is float f)
            {
                value = f;
                return true;
            }

            if (raw is decimal m)
            {
                value = (double) m;
                return true;
            }

            string text = raw.ToString ();
            return double.TryParse (
                text,
                NumberStyles.Float | NumberStyles.AllowThousands,
                CultureInfo.InvariantCulture,
                out value);
        }

        internal static bool TryToInt (object raw, out int value)
        {
            value = 0;

            if (raw is int direct)
            {
                value = direct;
                return true;
            }

            double asDouble;
            if (!TryToDouble (raw, out asDouble))
            {
                return false;
            }

            // Round rather than truncate: a slider that reports 2.9999999 for
            // what the user set to 3 must not become 2.
            double rounded = Math.Round (asDouble, MidpointRounding.AwayFromZero);
            if (rounded < int.MinValue || rounded > int.MaxValue)
            {
                return false;
            }

            value = (int) rounded;
            return true;
        }

        internal static bool TryToBool (object raw, out bool value)
        {
            value = false;

            if (raw == null)
            {
                return false;
            }

            if (raw is bool direct)
            {
                value = direct;
                return true;
            }

            double asDouble;
            if (TryToDouble (raw, out asDouble))
            {
                value = Math.Abs (asDouble) > double.Epsilon;
                return true;
            }

            return bool.TryParse (raw.ToString (), out value);
        }

        internal static bool TryToString (object raw, out string value)
        {
            value = raw == null ? null : raw.ToString ();
            return value != null;
        }

        /// <summary>
        /// Applies the author's declared range, if any.
        /// </summary>
        /// <remarks>
        /// Clamping rather than refusing is deliberate. The range crosses into
        /// the panel and the DG control already stops at it, so a value outside
        /// it means the request did not come from the panel — a script, a stale
        /// saved value, a second backend. Refusing would fail a solve the author
        /// can do nothing about; clamping solves the definition with the range
        /// the author declared, which is what the range is for.
        /// </remarks>
        internal double Clamp (double value)
        {
            if (m_minimum.HasValue && value < m_minimum.Value)
            {
                return m_minimum.Value;
            }

            if (m_maximum.HasValue && value > m_maximum.Value)
            {
                return m_maximum.Value;
            }

            return value;
        }

        internal static int CountOf (IEnumerable data)
        {
            if (data == null)
            {
                return 0;
            }

            int n = 0;
            foreach (object unused in data)
            {
                GC.KeepAlive (unused);
                n++;
            }

            return n;
        }
    }
}
