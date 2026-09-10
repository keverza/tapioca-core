using System;
using System.Collections.Generic;
using System.Drawing;

using Grasshopper.Kernel;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// What output collection looks for. The mirror of
    /// <see cref="ITapiocaInput"/>, and public for the same reason.
    /// </summary>
    public interface ITapiocaOutput
    {
        /// <summary>The stable id the panel keys this value by.</summary>
        string TapiocaOutputId { get; }

        string TapiocaOutputLabel { get; }

        /// <summary>The parameter whose volatile data IS the output.</summary>
        IGH_Param TapiocaOutputParam { get; }
    }

    /// <summary>
    /// Names one value a definition publishes back to Archicad.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ IT IS A NAME, NOT A TRANSPORT, AND THAT IS WHY SolveInstance DOES
    /// ALMOST NOTHING. The value does not leave through this component; the
    /// worker reads the input parameter's <c>VolatileData</c> after the solution
    /// completes, with its tree paths intact. Pushing the data out from inside
    /// SolveInstance would send it once per solution FRAGMENT and would flatten
    /// the tree on the way — HANDOFF-GHHost.md §9 wants an unsupported tree
    /// inspectable, and a flattened tree cannot be unflattened.
    /// </para>
    /// <para>
    /// ⚠️ THIS IS THE DATA OUTPUT, NOT THE COMMIT OUTPUT. Nothing here creates
    /// an Archicad element or plans one. The commit family is a separate
    /// component whose payload is a versioned semantic operation plan, validated
    /// natively and applied through ReplayBatch on an explicit press (§9).
    /// </para>
    /// </remarks>
    public class TapiocaOutputDataComponent : GH_Component, ITapiocaOutput
    {
        /// <summary>Where the id and the value live in Params.Input.</summary>
        private const int IdIndex = 0;

        private const int ValueIndex = 1;

        private string m_id = string.Empty;

        private string m_label = string.Empty;

        public TapiocaOutputDataComponent ()
            : base (
                "Tapioca Data Output",
                "TapiocaOut",
                "Publishes one named value from this definition back to Archicad's workflow panel.",
                "Tapioca",
                "Workflow")
        {
        }

        protected override void RegisterInputParams (GH_InputParamManager pManager)
        {
            pManager.AddTextParameter (
                "Id",
                "Id",
                "The stable id Archicad keys this output by. Must be unique in the definition.",
                GH_ParamAccess.item,
                string.Empty);
            pManager.AddGenericParameter (
                "Value",
                "V",
                "The value to publish. Its data tree is preserved.",
                GH_ParamAccess.tree);

            // Both optional so that an unwired component is a warning on the
            // canvas rather than an error that fails the whole solution: a
            // half-built definition is the normal state of one being authored.
            pManager[IdIndex].Optional = true;
            pManager[ValueIndex].Optional = true;
        }

        protected override void RegisterOutputParams (GH_OutputParamManager pManager)
        {
            pManager.AddTextParameter (
                "Id",
                "Id",
                "The id this output resolved to, so a definition can label its own panel.",
                GH_ParamAccess.item);
        }

        protected override void SolveInstance (IGH_DataAccess DA)
        {
            string id = string.Empty;
            DA.GetData (IdIndex, ref id);

            // The nickname is the fallback, exactly as it is for inputs: an
            // author who renames the component on the canvas has named the
            // output, and making them type the same word twice is friction with
            // no benefit.
            m_id = string.IsNullOrWhiteSpace (id) ? (NickName ?? string.Empty).Trim () : id.Trim ();
            m_label = string.IsNullOrWhiteSpace (NickName) ? m_id : NickName;

            if (string.IsNullOrEmpty (m_id))
            {
                AddRuntimeMessage (
                    GH_RuntimeMessageLevel.Warning,
                    "This output has no id, so Archicad has nothing to key it by. Set Id, or rename the "
                        + "component.");
            }

            DA.SetData (0, m_id);
        }

        /// <summary>
        /// The id Archicad keys this output by.
        /// </summary>
        /// <remarks>
        /// ⚠️ THE NICKNAME IS THE FALLBACK HERE AND NOT ONLY IN SolveInstance,
        /// AND THAT IS THE FIX FOR AN OUTPUT THAT NEEDED A PANEL TO HAVE A
        /// NAME. m_id is only filled while solving, so before the first
        /// solution this answered with an empty string -- which made the panel
        /// key the value by the component's LABEL, or by nothing. Renaming the
        /// component on the canvas is how a Grasshopper author names anything;
        /// requiring a text panel wired into Id as well was asking for the same
        /// word twice.
        ///
        /// The Id input still WINS when it has something in it: an author who
        /// wants an id that is not the nickname has said so explicitly.
        /// </remarks>
        public string TapiocaOutputId
        {
            get
            {
                if (!string.IsNullOrWhiteSpace (m_id))
                {
                    return m_id;
                }

                return (NickName ?? string.Empty).Trim ();
            }
        }

        public string TapiocaOutputLabel
        {
            get { return string.IsNullOrEmpty (m_label) ? m_id : m_label; }
        }

        /// <summary>
        /// The Value parameter, whose volatile data the worker reads after the
        /// solution completes.
        /// </summary>
        /// <remarks>
        /// Guarded rather than indexed blind: a document being deserialised, or
        /// one whose components were edited by an older build, can reach here
        /// with fewer parameters than this component registers, and an
        /// IndexOutOfRange inside collection would lose the whole solution's
        /// outputs over one broken component.
        /// </remarks>
        public IGH_Param TapiocaOutputParam
        {
            get
            {
                IList<IGH_Param> inputs = Params.Input;
                return inputs != null && inputs.Count > ValueIndex ? inputs[ValueIndex] : null;
            }
        }

        protected override Bitmap Icon
        {
            get { return null; }
        }

        public override Guid ComponentGuid
        {
            get { return new Guid ("2f6c1a94-9f0d-4f2e-9a3b-5c7e11d84b60"); }
        }
    }
}
