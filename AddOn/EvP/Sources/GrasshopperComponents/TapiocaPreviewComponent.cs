using System;
using System.Collections.Generic;
using System.Drawing;

using Grasshopper.Kernel;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// Captures geometry for Archicad's viewport. Mechanism 1 of the three the
    /// handoff describes, and deliberately the first one built.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ EXPLICIT AND OPT-IN, WHICH IS THE POINT. What Archicad previews is
    /// exactly what was wired into this component — not everything the canvas
    /// happens to be drawing. That is what a Player definition wants: an author
    /// decides what the building model shows, rather than every intermediate
    /// result leaking into someone's viewport. The document-wide sweep that gives
    /// canvas parity is mechanism 2 and comes later, off by default.
    /// </para>
    /// <para>
    /// It also depends on no Grasshopper preview internals — only on the parameter
    /// data it is handed — which makes it the mechanism least likely to break
    /// across a Rhino service release, and the one buildable before the Grasshopper
    /// SDK reference lands.
    /// </para>
    /// <para>
    /// ⚠️ WHAT THIS DOES AND DOES NOT REACH. The capture, the delta and the
    /// TRANSPORT are complete: a batch is framed, its bulk goes through shared
    /// memory, and Archicad's GhPreviewCache holds the result. What is drawn from
    /// that cache is the Diligent and plan layers' job. Read the Delta output as
    /// evidence that a batch was ACCEPTED, not that a picture appeared.
    /// </para>
    /// <para>
    /// ⚠️ THE PREVIEW APPEARS IN TAPIOCA'S 3D VIEWER, NOT IN ARCHICAD'S OWN 3D
    /// WINDOW. The drawing happens inside the Diligent viewport's frame loop, so
    /// that viewport has to be running -- Tapioca > Tapioca 3D Viewer -- either as
    /// its own palette or as the transparent overlay over Archicad's 3D view.
    /// With it closed the batch still arrives and is still cached; there is
    /// simply nothing on screen to draw it.
    /// </para>
    /// <para>
    /// ⚠️ A FAILED SEND DROPS THE MIRROR RATHER THAN RETRYING. The diff was
    /// computed against a mirror that has already advanced, so after a failure
    /// the two sides disagree and every later delta compounds it. Forgetting the
    /// mirror costs one full batch; retrying against it costs a viewport that is
    /// quietly wrong for the rest of the session.
    /// </para>
    /// <para>
    /// ⚠️ CAPTURE MUST NEVER CAUSE A SOLVE. This reads the data it is given inside
    /// its own SolveInstance and expires nothing. A capture path that marks
    /// something dirty in order to draw it is a feedback loop that presents as
    /// "Archicad makes Grasshopper run continuously", and the cause is very hard to
    /// see from that symptom.
    /// </para>
    /// </remarks>
    public class TapiocaPreviewComponent : GH_Component
    {
        /// <summary>
        /// The mirror is per COMPONENT INSTANCE, so two preview components on one
        /// canvas keep separate diffs and neither reports the other's geometry as
        /// removed. Identity already includes the component guid, so the host's
        /// cache stays coherent across both.
        /// </summary>
        private readonly PreviewMirror _mirror = new PreviewMirror();

        public TapiocaPreviewComponent()
            : base(
                "Tapioca Preview",
                "TapiocaPrev",
                "Captures geometry for Archicad's viewport. What is wired here is what Archicad shows.",
                "Tapioca",
                "Archicad")
        {
        }

        protected override void RegisterInputParams(GH_InputParamManager pManager)
        {
            pManager.AddGenericParameter(
                "Geometry",
                "G",
                "Geometry to preview in Archicad. Meshes, breps, surfaces, curves, points, planes and "
                + "vectors are understood; anything else is counted as unsupported.",
                GH_ParamAccess.list);
            // ⚠️ HIDES GRASSHOPPER'S OWN PREVIEW OF THIS INPUT, NOT TAPIOCA'S.
            // HideParameter sets IGH_PreviewObject.Hidden on the input parameter
            // (Grasshopper/Kernel/GH_Component.cs), which is exactly what stock
            // Custom Preview does with its geometry input. Without it the wired
            // geometry is drawn TWICE — once by the parameter on Grasshopper's
            // canvas preview and once by Archicad — and the duplicate reads as a
            // z-fighting artefact rather than as two previews of one thing.
            pManager.HideParameter(0);
            pManager.AddTextParameter(
                "Target",
                "T",
                "Which Archicad window this geometry is for: \"3D\", \"Plan\" or \"Both\". Tapir's "
                + "GetCurrentWindowType output (FloorPlan, 3DModel, Section, Layout) can be wired "
                + "straight in.",
                GH_ParamAccess.item,
                "3D");
            pManager.AddBooleanParameter(
                "Visible",
                "V",
                "Clear to hide this component's preview in Archicad without removing it.",
                GH_ParamAccess.item,
                true);
            pManager[0].Optional = true;
        }

        protected override void RegisterOutputParams(GH_OutputParamManager pManager)
        {
            pManager.AddIntegerParameter(
                "Primitives", "P", "How many preview primitives this capture produced.", GH_ParamAccess.item);
            pManager.AddTextParameter(
                "Delta", "D", "What changed since the last solve.", GH_ParamAccess.item);
        }

        protected override void SolveInstance(IGH_DataAccess DA)
        {
            List<object> items = new List<object>();
            DA.GetDataList(0, items);

            string target = "3D";
            DA.GetData(1, ref target);

            PreviewSurface surface;
            if (!TryReadSurface(target, out surface))
            {
                // ⚠️ NOT A SILENT FALLBACK TO 3D. Plan linework quietly drawn in
                // the model window looks like a bug in the definition, and the
                // author would hunt the geometry rather than the spelling.
                AddRuntimeMessage(
                    GH_RuntimeMessageLevel.Error,
                    "\"" + target + "\" is not a preview target. Use 3D, Plan or Both — or wire Tapir's "
                    + "GetCurrentWindowType in, which reports 3DModel and FloorPlan.");
                return;
            }

            bool visible = true;
            DA.GetData(2, ref visible);

            // ⚠️ ASKED BEFORE ANYTHING IS CONVERTED, AND THAT IS THE WHOLE POINT
            // OF THE CAPABILITY GATE. Tessellating a definition's breps and then
            // discarding the batch would put the entire cost of preview on every
            // solve for a user who has it switched off, or who is running this
            // canvas outside Archicad altogether.
            if (!TapiocaPreviewBridge.Available)
            {
                DA.SetData(0, 0);
                DA.SetData(1, "not sent");
                // ⚠️ WARNING, NOT Remark. Grasshopper does not DISPLAY remarks:
                // there is no balloon and no colour change, and the text is only
                // reachable by hovering a component that looks perfectly healthy.
                // Every state that means "your preview is not reaching Archicad"
                // has to change how the component LOOKS, or the first report is
                // "nothing appears and it does not say why" -- which is exactly
                // what happened.
                AddRuntimeMessage(
                    GH_RuntimeMessageLevel.Warning,
                    "Archicad is not taking preview from this Grasshopper. Open Grasshopper from Archicad with "
                    + "Tapioca > Grasshopper Editor (a Grasshopper started any other way has no Archicad to talk "
                    + "to), and make sure the add-on and the worker were rebuilt and redeployed together.");
                return;
            }

            Guid parameterGuid = Params.Input.Count > 0 ? Params.Input[0].InstanceGuid : InstanceGuid;

            // ⚠️ THE BRANCH COMES FROM THE DATA ACCESS, NOT FROM A COUNTER. Identity
            // has to survive a re-solve that reorders nothing but arrives on a
            // different path, or the host sees a Remove plus an Add for geometry
            // that did not move.
            uint branchHash = PreviewHash.Branch(DA.ParameterTargetPath(0).Indices);

            List<PreviewPrimitive> captured = new List<PreviewPrimitive>();
            int unsupported = 0;
            for (int index = 0; index < items.Count; index++)
            {
                PreviewPrimitive primitive = PreviewConvert.Convert(
                    items[index], InstanceGuid, parameterGuid, branchHash, (uint)index, surface);
                if (primitive == null)
                {
                    unsupported++;
                    continue;
                }

                if (!visible)
                {
                    primitive.Flags &= ~PreviewFlags.Visible;
                    // The content hash covers geometry, not flags, so it is left
                    // alone here on purpose: hiding must diff as Visibility, never
                    // as Changed.
                }

                captured.Add(primitive);
            }

            PreviewBatch batch = _mirror.Diff(captured);

            DA.SetData(0, captured.Count);
            DA.SetData(1, batch.Describe());

            if (unsupported > 0)
            {
                // Some unsupported items among good ones is ordinary -- a
                // definition carries numbers and strings. ALL of them unsupported
                // means nothing will ever appear, and that must be visible.
                AddRuntimeMessage(
                    captured.Count == 0 ? GH_RuntimeMessageLevel.Warning : GH_RuntimeMessageLevel.Remark,
                    unsupported + " item(s) were not geometry Tapioca previews, and were skipped.");
            }

            Send(batch);

            // ⚠️ SAID ON THE COMPONENT, POSITIVELY, EVEN WHEN EVERYTHING WORKED.
            // "Sent" and "drawn" are different claims and only Archicad knows the
            // second: the kinds this build does not draw yet cross the wire
            // perfectly and then land in a viewport that has no code for them. A
            // component that reports success while the screen stays empty sends
            // the user looking at their definition, which is the one place the
            // fault is not.
            Message = captured.Count + " sent";
        }

        /// <summary>
        /// Frames the diff and hands it to the bridge, and says on the canvas
        /// when it could not go.
        /// </summary>
        /// <remarks>
        /// ⚠️ AN EMPTY BATCH SENDS NOTHING AT ALL. Steady state — a definition
        /// re-solving with nothing this component produced having changed — must
        /// cost ZERO BYTES, not an empty batch's framing and an ack round trip.
        /// It is the commonest case on a slider drag.
        /// </remarks>
        private void Send(PreviewBatch batch)
        {
            if (batch.IsEmpty)
            {
                return;
            }

            PreviewChannel.PreviewWireBatch wire = PreviewChannel.EncodeBatch(batch, TapiocaPreviewBridge.Epoch);
            string error = TapiocaPreviewBridge.Send(wire);
            if (string.IsNullOrEmpty(error))
            {
                return;
            }

            // ⚠️ THE MIRROR GOES WITH THE FAILURE. The diff was computed against
            // it and the batch never arrived, so it now describes a cache
            // Archicad does not have; the next solve must send a full batch
            // rather than a delta against a fiction.
            _mirror.DropAll();
            AddRuntimeMessage(
                GH_RuntimeMessageLevel.Warning,
                "This capture did not reach Archicad, and the next solve will resend all of it: " + error);
        }

        /// <summary>
        /// Reads a target the author typed, or one Tapir's GetCurrentWindowType
        /// produced.
        /// </summary>
        /// <remarks>
        /// ⚠️ TAPIR'S VOCABULARY IS ACCEPTED ON PURPOSE, AND IT IS THE WHOLE
        /// POINT OF THIS BEING TEXT. GetCurrentWindowType answers "FloorPlan",
        /// "3DModel", "Section" or "Layout"; wiring it into this input is how an
        /// author gets "preview wherever I am working" as a visible, editable
        /// choice on the canvas rather than as hidden state in the add-on.
        ///
        /// Section and Layout are recognised and REFUSED rather than ignored:
        /// Tapioca has no preview surface for either yet, and returning false
        /// makes SolveInstance say so instead of drawing them somewhere wrong.
        /// </remarks>
        private static bool TryReadSurface(string value, out PreviewSurface surface)
        {
            surface = PreviewSurface.Model3D;
            if (string.IsNullOrWhiteSpace(value))
            {
                return true;
            }

            switch (value.Trim().ToUpperInvariant())
            {
                case "3D":
                case "3DMODEL":
                case "MODEL":
                    surface = PreviewSurface.Model3D;
                    return true;

                case "PLAN":
                case "FLOORPLAN":
                case "2D":
                    surface = PreviewSurface.FloorPlan;
                    return true;

                case "BOTH":
                case "ALL":
                    surface = PreviewSurface.Both;
                    return true;

                default:
                    return false;
            }
        }

        /// <summary>
        /// Forgets the mirror when the component leaves the document, so that
        /// re-adding it does not diff against a host cache that was dropped with it.
        /// </summary>
        public override void RemovedFromDocument(GH_Document document)
        {
            // ⚠️ BOTH MIRRORS, NOT JUST THIS ONE. Forgetting only the worker's
            // side would leave Archicad holding this component's geometry with
            // nothing left in the definition that could ever remove it — a
            // preview that cannot be got rid of short of restarting the worker.
            _mirror.DropAll();
            TapiocaPreviewBridge.DropAll("a Tapioca Preview component left the document");
            base.RemovedFromDocument(document);
        }

        public override GH_Exposure Exposure
        {
            get { return GH_Exposure.primary; }
        }

        protected override Bitmap Icon
        {
            get { return null; }
        }

        public override Guid ComponentGuid
        {
            get { return new Guid("7C4E1B92-3A6D-4F58-9E21-5D8B0A7C36F4"); }
        }
    }
}
