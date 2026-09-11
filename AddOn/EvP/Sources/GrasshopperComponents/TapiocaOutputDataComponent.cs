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
        /// <summary>Where the value lives in Params.Input.</summary>
        private const int ValueIndex = 0;

        /// <summary>
        /// The nickname a component has before anybody renames it, and therefore
        /// the one id that means "unnamed".
        /// </summary>
        private const string UnnamedNickName = "Tapioca Data Output";

        public TapiocaOutputDataComponent ()
            : base (
                "Tapioca Data Output",
                "TapiocaOut",
                "Publishes one named value from this definition back to Archicad's workflow panel. Rename it to "
                    + "name the output.",
                "Tapioca",
                "Workflow")
        {
            // ⚠️ THE FULL NAME ON THE CANVAS, DELIBERATELY, because the name IS
            // the output's id now. A short nickname would publish a value into
            // Archicad's panel under a word the author never chose and cannot see
            // without clicking the component.
            NickName = UnnamedNickName;
        }

        /// <remarks>
        /// ⚠️ ONE INPUT AND NO OUTPUT, LIKE ContextBake AND ContextPrint.
        /// Those are the components Grasshopper Player and rhino.compute already
        /// use to mean "this value leaves the definition", and they take a value
        /// and give nothing back: there is nothing downstream of leaving. The Id
        /// input and the Id output this component used to have were both
        /// ceremony -- the id is the component's NAME, and echoing it back onto
        /// the canvas invited a wire that could only loop information the author
        /// had already typed.
        /// </remarks>
        protected override void RegisterInputParams (GH_InputParamManager pManager)
        {
            pManager.AddGenericParameter (
                "Value",
                "Value",
                "The value to publish. Its data tree is preserved.",
                GH_ParamAccess.tree);

            // Optional so that an unwired component is a warning on the canvas
            // rather than an error that fails the whole solution: a half-built
            // definition is the normal state of one being authored.
            pManager[ValueIndex].Optional = true;
        }

        protected override void RegisterOutputParams (GH_OutputParamManager pManager)
        {
            // Deliberately none. See RegisterInputParams.
        }

        protected override void SolveInstance (IGH_DataAccess DA)
        {
            // ⚠️ NOTHING IS READ AND NOTHING IS WRITTEN HERE. The worker reads
            // the input parameter's VolatileData after the solution completes,
            // tree intact; see the class remarks for why that cannot happen from
            // inside SolveInstance. This override exists only to say when the
            // component is not usable yet.
            if (string.Equals (TapiocaOutputId, UnnamedNickName, StringComparison.Ordinal))
            {
                AddRuntimeMessage (
                    GH_RuntimeMessageLevel.Warning,
                    "Rename this component to name the output. Archicad keys the value by that name, and every "
                        + "unrenamed output in one definition would claim the same one.");
            }
            else if (string.IsNullOrWhiteSpace (TapiocaOutputId))
            {
                AddRuntimeMessage (
                    GH_RuntimeMessageLevel.Warning,
                    "This output has no name, so Archicad has nothing to key it by.");
            }
        }

        /// <summary>
        /// The id Archicad keys this output by: the component's name.
        /// </summary>
        /// <remarks>
        /// ⚠️ THE NAME, WITH NOTHING ELSE CONSULTED, AND IT IS AVAILABLE
        /// BEFORE THE FIRST SOLVE. The panel asks for the schema before anything
        /// has solved, so an id that was only computed inside SolveInstance
        /// answered with an empty string at exactly the moment it was needed.
        /// Renaming a component is how a Grasshopper author names anything; there
        /// is no second source of truth left to reconcile.
        /// </remarks>
        public string TapiocaOutputId
        {
            get { return (NickName ?? string.Empty).Trim (); }
        }

        public string TapiocaOutputLabel
        {
            get { return TapiocaOutputId; }
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
        ///
        /// ⚠️ A DEFINITION SAVED WITH THE OLD TWO-INPUT SHAPE STILL WORKS.
        /// Its Value was the SECOND parameter, so the LAST input is taken rather
        /// than the first: for this build's one-input component they are the same
        /// parameter, and for an older document they are the one that held the
        /// value. The stale Id parameter is left alone -- deleting an author's
        /// wired parameter on load would be worse than ignoring it.
        /// </remarks>
        public IGH_Param TapiocaOutputParam
        {
            get
            {
                IList<IGH_Param> inputs = Params.Input;
                if (inputs == null || inputs.Count == 0)
                {
                    return null;
                }

                return inputs[inputs.Count - 1];
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
