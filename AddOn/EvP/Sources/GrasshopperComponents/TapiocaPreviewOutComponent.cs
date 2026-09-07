using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.Text;

using Grasshopper.Kernel;
using Grasshopper.Kernel.Types;

using Rhino.Geometry;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// States a mesh as plain numbers so the rhino.compute backend can return it.
    /// </summary>
    /// <remarks>
    /// ⚠️ THIS IS THE COMPUTE BACKEND'S PREVIEW PATH, AND IT IS NOT
    /// TapiocaPreviewComponent. That one collects primitives and pushes them
    /// through PreviewChannel into the worker's shared-memory segment, which is
    /// a transport rhino.compute does not have: compute is stateless HTTP and
    /// returns only the VALUES of output parameters. So this component produces
    /// a value instead of sending one.
    ///
    /// ⚠️ EXPLICIT FLOAT ARRAYS, NEVER A SERIALIZED RHINOCOMMON OBJECT. Compute
    /// would happily serialize a Mesh for us, as an opennurbs archive wrapped in
    /// base64 — and then EvP.apx would have to link opennurbs to draw a preview,
    /// and the bytes would not match what the pipe backend already sends. The
    /// existing GhPreviewProtocol carries float32 vertices and indices; this
    /// emits the same numbers as JSON text, so both backends feed one cache.
    /// SPEC-RhinoCompute.md records this as the decision that closes the open
    /// question about compute's geometry encoding.
    ///
    /// ⚠️ PREVIEW GEOMETRY IS NOT BIM GEOMETRY. Nothing downstream may turn what
    /// this emits into an Archicad element; element creation stays a deliberate
    /// Tapioca operation validated on the host side.
    /// </remarks>
    public class TapiocaPreviewOutComponent : GH_Component
    {
        public TapiocaPreviewOutComponent()
            : base(
                "Tapioca Preview Out",
                "TapPrevOut",
                "States geometry as vertices and indices for Tapioca to draw. Wire this to a definition output.",
                "Tapioca",
                "Outputs")
        {
        }

        protected override void RegisterInputParams(GH_InputParamManager pManager)
        {
            pManager.AddGeometryParameter(
                "Geometry",
                "G",
                "A mesh, or a Brep or surface that will be meshed with the document's render settings.",
                GH_ParamAccess.list);

            pManager.AddIntegerParameter(
                "Id",
                "I",
                "Stable id for this preview. Keeping it constant across solves lets Tapioca update buffers instead of rebuilding them.",
                GH_ParamAccess.item,
                0);

            pManager[1].Optional = true;
        }

        protected override void RegisterOutputParams(GH_OutputParamManager pManager)
        {
            pManager.AddTextParameter(
                "Preview",
                "P",
                "The preview payload. Wire it to whatever parameter the definition exposes as an output.",
                GH_ParamAccess.list);
        }

        protected override void SolveInstance(IGH_DataAccess DA)
        {
            List<IGH_GeometricGoo> geometry = new List<IGH_GeometricGoo>();
            if (!DA.GetDataList(0, geometry))
            {
                return;
            }

            int baseId = 0;
            DA.GetData(1, ref baseId);

            List<string> payloads = new List<string>();
            int emitted = 0;

            foreach (IGH_GeometricGoo goo in geometry)
            {
                if (goo == null)
                {
                    continue;
                }

                Mesh mesh = ToMesh(goo);
                if (mesh == null)
                {
                    AddRuntimeMessage(
                        GH_RuntimeMessageLevel.Warning,
                        "Skipped a " + goo.TypeName + ": Tapioca preview carries meshes, and this could not be meshed.");
                    continue;
                }

                // A mesh with n-gons or quads indexes badly on a GPU that only
                // draws triangles, and the host must not be the place that
                // discovers it. Triangulate here, where the mesh is still a
                // mesh.
                mesh.Faces.ConvertQuadsToTriangles();
                mesh.Compact();

                if (mesh.Vertices.Count == 0 || mesh.Faces.Count == 0)
                {
                    continue;
                }

                payloads.Add(Encode(mesh, baseId + emitted));
                emitted++;
            }

            DA.SetDataList(0, payloads);
        }

        private static Mesh ToMesh(IGH_GeometricGoo goo)
        {
            GH_Mesh ghMesh = goo as GH_Mesh;
            if (ghMesh != null && ghMesh.Value != null)
            {
                return ghMesh.Value.DuplicateMesh();
            }

            Brep brep = null;

            GH_Brep ghBrep = goo as GH_Brep;
            if (ghBrep != null)
            {
                brep = ghBrep.Value;
            }

            GH_Surface ghSurface = goo as GH_Surface;
            if (ghSurface != null)
            {
                brep = ghSurface.Value;
            }

            GH_Box ghBox = goo as GH_Box;
            if (ghBox != null && ghBox.Value.IsValid)
            {
                brep = ghBox.Value.ToBrep();
            }

            if (brep == null)
            {
                return null;
            }

            Mesh[] parts = Mesh.CreateFromBrep(brep, MeshingParameters.Default);
            if (parts == null || parts.Length == 0)
            {
                return null;
            }

            Mesh joined = new Mesh();
            foreach (Mesh part in parts)
            {
                if (part != null)
                {
                    joined.Append(part);
                }
            }

            return joined.Vertices.Count > 0 ? joined : null;
        }

        /// <summary>
        /// The wire shape RhinoComputeProtocol.cpp parses.
        /// </summary>
        /// <remarks>
        /// The "tapiocaPreview" marker is load-bearing rather than decorative:
        /// a definition is free to output anything, and the host must be able to
        /// tell a preview payload from a serialized RhinoCommon object without
        /// parsing either. Its absence means "not for me", not "malformed".
        /// </remarks>
        private static string Encode(Mesh mesh, int id)
        {
            StringBuilder sb = new StringBuilder(mesh.Vertices.Count * 24);
            sb.Append("{\"tapiocaPreview\":1,\"id\":").Append(id.ToString(CultureInfo.InvariantCulture));

            sb.Append(",\"vertices\":[");
            for (int i = 0; i < mesh.Vertices.Count; i++)
            {
                Point3f p = mesh.Vertices[i];
                if (i > 0)
                {
                    sb.Append(',');
                }

                Append(sb, p.X);
                sb.Append(',');
                Append(sb, p.Y);
                sb.Append(',');
                Append(sb, p.Z);
            }

            sb.Append(']');

            // Normals are optional on the wire, and an absent set is cheaper than
            // a fabricated one: the host can compute face normals, but it cannot
            // undo wrong vertex normals.
            if (mesh.Normals.Count == mesh.Vertices.Count)
            {
                sb.Append(",\"normals\":[");
                for (int i = 0; i < mesh.Normals.Count; i++)
                {
                    Vector3f n = mesh.Normals[i];
                    if (i > 0)
                    {
                        sb.Append(',');
                    }

                    Append(sb, n.X);
                    sb.Append(',');
                    Append(sb, n.Y);
                    sb.Append(',');
                    Append(sb, n.Z);
                }

                sb.Append(']');
            }

            sb.Append(",\"indices\":[");
            bool first = true;
            foreach (MeshFace face in mesh.Faces)
            {
                if (!face.IsTriangle)
                {
                    continue;
                }

                if (!first)
                {
                    sb.Append(',');
                }

                sb.Append(face.A.ToString(CultureInfo.InvariantCulture)).Append(',');
                sb.Append(face.B.ToString(CultureInfo.InvariantCulture)).Append(',');
                sb.Append(face.C.ToString(CultureInfo.InvariantCulture));
                first = false;
            }

            sb.Append("]}");
            return sb.ToString();
        }

        // "R" round-trips a float through its shortest exact form, and
        // InvariantCulture keeps a machine set to a comma decimal separator from
        // writing "1,5" into a JSON array — which would parse as two numbers and
        // silently shift every coordinate after it.
        private static void Append(StringBuilder sb, float value)
        {
            sb.Append(value.ToString("R", CultureInfo.InvariantCulture));
        }

        protected override Bitmap Icon
        {
            get { return null; }
        }

        public override GH_Exposure Exposure
        {
            get { return GH_Exposure.primary; }
        }

        public override Guid ComponentGuid
        {
            get { return new Guid("5c93f8a1-64b7-4e02-a3d5-8f71b920ce46"); }
        }
    }
}
