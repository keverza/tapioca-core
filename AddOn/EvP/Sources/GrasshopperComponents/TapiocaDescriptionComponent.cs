using System;
using System.Collections;
using System.Collections.Generic;
using System.Text;

using Grasshopper.Kernel;
using Grasshopper.Kernel.Data;
using Grasshopper.Kernel.Special;
using Grasshopper.Kernel.Types;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// Carries the definition's own description into Archicad's panel.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ IT IS READ WITHOUT SOLVING, WHICH IS WHY IT READS ITS SOURCES AND NOT
    /// ITS DATA. The panel asks for a schema the moment a definition loads —
    /// before any solve — so this component's input parameter has no volatile
    /// data yet and a panel wired into it has not been collected. Waiting for a
    /// solve would mean a definition showed no description until after it had
    /// been run once, which is exactly backwards: the description is what tells
    /// the user whether to run it.
    /// </para>
    /// <para>
    /// So <see cref="Read"/> looks in three places, nearest first: the
    /// parameter's own persistent text (someone typed into the component), then
    /// each wired source's <c>GH_Panel.UserText</c> (the ordinary case — a panel
    /// with the words in it), then any persistent data a wired source holds.
    /// The one thing it will not do is force a collection: <c>CollectData</c> on
    /// a document that has not solved expires state the solve is about to need.
    /// </para>
    /// <para>
    /// ⚠️ NOT AN INPUT AND NOT AN OUTPUT. It implements neither
    /// <see cref="ITapiocaInput"/> nor <see cref="ITapiocaOutput"/>, so
    /// discovery ignores it and it occupies no row in the panel. It is metadata
    /// about the definition, like its name.
    /// </para>
    /// </remarks>
    public class TapiocaDescriptionComponent : GH_Component
    {
        /// <summary>Where the text lives in Params.Input.</summary>
        private const int TextIndex = 0;

        /// <summary>
        /// How much description is carried. A paragraph is the intent; a
        /// definition that wires its whole report in here gets the beginning of
        /// it rather than a refusal.
        /// </summary>
        private const int MaxDescriptionChars = 2000;

        public TapiocaDescriptionComponent ()
            : base (
                "Tapioca Description",
                "TapiocaDoc",
                "Describes this definition for Archicad's panel. Wire a panel of text into it.",
                "Tapioca",
                "Workflow")
        {
        }

        public override Guid ComponentGuid
        {
            get { return new Guid ("2f7a4c31-7d64-4c1f-9d2a-6b1f0c85e4a7"); }
        }

        public override GH_Exposure Exposure
        {
            get { return GH_Exposure.secondary; }
        }

        protected override void RegisterInputParams (GH_InputParamManager pManager)
        {
            pManager.AddTextParameter (
                "Description",
                "Doc",
                "What this definition does, shown in Archicad's palette above the inputs.",
                GH_ParamAccess.item,
                string.Empty);

            // Optional so an unwired component is a warning on the canvas rather
            // than an error that fails the whole solution: a half-built
            // definition is the normal state of one being authored.
            pManager[TextIndex].Optional = true;
        }

        protected override void RegisterOutputParams (GH_OutputParamManager pManager)
        {
            // None. The value does not flow anywhere in Grasshopper; it is read
            // off the document by the worker. An output would suggest it was
            // part of the definition's data flow.
        }

        protected override void SolveInstance (IGH_DataAccess DA)
        {
            string text = string.Empty;
            DA.GetData (TextIndex, ref text);

            if (string.IsNullOrWhiteSpace (text) && string.IsNullOrWhiteSpace (Read (this)))
            {
                AddRuntimeMessage (
                    GH_RuntimeMessageLevel.Warning,
                    "This component has no text, so the definition will show no description. Wire a panel into it.");
            }
        }

        /// <summary>
        /// The definition's description, or an empty string.
        /// </summary>
        /// <remarks>
        /// Static and document-wide: the first component with text wins, and a
        /// second one is reported by <see cref="Problems"/> rather than
        /// concatenated. Two descriptions is an authoring mistake, and joining
        /// them would produce a paragraph neither author wrote.
        /// </remarks>
        internal static string Describe (GH_Document document, IList<string> problems)
        {
            if (document == null)
            {
                return string.Empty;
            }

            string found = string.Empty;
            int seen = 0;

            foreach (IGH_DocumentObject obj in document.Objects)
            {
                TapiocaDescriptionComponent component = obj as TapiocaDescriptionComponent;
                if (component == null || component.Locked)
                {
                    continue;
                }

                seen++;
                string text = Read (component);
                if (found.Length == 0 && text.Length > 0)
                {
                    found = text;
                }
            }

            if (seen > 1 && problems != null)
            {
                problems.Add (
                    "This definition has " + seen.ToString ()
                    + " Tapioca Description components. The first one with text is used; delete the others.");
            }

            return found;
        }

        /// <summary>
        /// One component's text, without solving anything. See the class remarks
        /// for why the sources are read rather than the data.
        /// </summary>
        private static string Read (TapiocaDescriptionComponent component)
        {
            try
            {
                IGH_Param param = component.Params.Input[TextIndex];

                string own = FirstText (param);
                if (own.Length > 0)
                {
                    return Clip (own);
                }

                foreach (IGH_Param source in param.Sources)
                {
                    // A panel is what an author reaches for to write a
                    // paragraph, and its text is a plain property -- no solve,
                    // no data tree, no conversion.
                    GH_Panel panel = source as GH_Panel;
                    if (panel != null && !string.IsNullOrWhiteSpace (panel.UserText))
                    {
                        return Clip (panel.UserText);
                    }

                    string upstream = FirstText (source);
                    if (upstream.Length > 0)
                    {
                        return Clip (upstream);
                    }
                }
            }
            catch (Exception)
            {
                // A description is a courtesy. Losing one costs a paragraph;
                // letting the exception out would cost the whole schema.
            }

            return string.Empty;
        }

        /// <summary>The first text a parameter holds, volatile then persistent.</summary>
        private static string FirstText (IGH_Param param)
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
                        string text = TextOf (item);
                        if (text.Length > 0)
                        {
                            return text;
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
                    string text = TextOf (goo);
                    if (text.Length > 0)
                    {
                        return text;
                    }
                }
            }

            return string.Empty;
        }

        private static string TextOf (object item)
        {
            if (item == null)
            {
                return string.Empty;
            }

            GH_String text = item as GH_String;
            if (text != null)
            {
                return text.Value ?? string.Empty;
            }

            IGH_Goo goo = item as IGH_Goo;
            return (goo == null ? item.ToString () : goo.ToString ()) ?? string.Empty;
        }

        private static string Clip (string text)
        {
            string trimmed = text.Trim ();
            if (trimmed.Length <= MaxDescriptionChars)
            {
                return trimmed;
            }

            return trimmed.Substring (0, MaxDescriptionChars) + "...";
        }
    }
}
