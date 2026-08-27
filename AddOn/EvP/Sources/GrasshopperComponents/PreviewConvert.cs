using System;
using System.Collections.Generic;

using Grasshopper.Kernel.Types;

using Rhino.Geometry;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// Turns what a definition produced into preview primitives. The ONLY file in
    /// this package that names a Rhino geometry type.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ RESULTS, NOT DRAW CALLS. This does not mirror
    /// <c>Rhino.Display.DisplayPipeline</c>, and the handoff says why at length:
    /// it is immediate-mode, so mirroring it means retransmitting everything every
    /// solve with no identity to diff against; its calls depend on Rhino display
    /// state Tapioca does not control; and it is a large third-party contract to
    /// pin on the hot path. What crosses is what the definition MEANT — a plane,
    /// an arrow, a mesh — never how Rhino would have drawn it.
    /// </para>
    /// <para>
    /// ⚠️ THE HOST BUILDS THE VIEW-DEPENDENT PARTS. A plane crosses as nine
    /// doubles and becomes three constant-pixel-width axes in Archicad. Sending
    /// those axes as polylines would triple the payload, resample on every zoom,
    /// and throw away the fact that it is a gizmo the moment it arrived. Same for
    /// arrow heads and text quads. Convert to meaning here; build geometry at the
    /// camera there.
    /// </para>
    /// <para>
    /// ⚠️ TESSELLATION IS A PREVIEW TOLERANCE AND NOTHING DOWNSTREAM MAY TREAT IT
    /// AS BIM GEOMETRY. Nothing in this path creates an Archicad element or
    /// carries a Brep. It is display geometry, one way, for looking at.
    /// </para>
    /// <para>
    /// ⚠️ CONVERSION MUST NOT EXPIRE ANYTHING. Reading data to draw it and thereby
    /// marking a component dirty is a feedback loop that presents as "Archicad
    /// makes Grasshopper run continuously", and the cause is very hard to see from
    /// that symptom. Everything here reads; nothing writes back into the document.
    /// </para>
    /// </remarks>
    internal static class PreviewConvert
    {
        /// <summary>
        /// Curve sampling density. A preview number, not a BIM one: enough that an
        /// arc does not read as a polygon at working zoom, few enough that a
        /// definition full of curves does not saturate the pipe.
        /// </summary>
        private const int CurveSamples = 64;

        /// <summary>
        /// Converts one item. Returns null when the item is not something preview
        /// represents — which is normal and not an error; a definition carries
        /// plenty of numbers and strings that are not geometry.
        /// </summary>
        internal static PreviewPrimitive Convert(
            object item,
            Guid componentGuid,
            Guid parameterGuid,
            uint branchHash,
            uint itemIndex)
        {
            PreviewPrimitive primitive = Build(Unwrap(item));
            if (primitive == null)
            {
                return null;
            }

            primitive.ComponentGuid = componentGuid;
            primitive.ParameterGuid = parameterGuid;
            primitive.BranchHash = branchHash;
            primitive.ItemIndex = itemIndex;
            primitive.Flags = PreviewFlags.Visible | PreviewFlags.DepthTest;
            primitive.Id = PreviewHash.Identity(componentGuid, parameterGuid, branchHash, itemIndex);
            primitive.ContentHash = PreviewHash.Content(primitive);
            return primitive;
        }

        /// <summary>
        /// Grasshopper hands out IGH_Goo wrappers; the geometry is inside. Kept
        /// separate so the type switch below reads as geometry rather than as
        /// unwrapping.
        /// </summary>
        private static object Unwrap(object item)
        {
            IGH_Goo goo = item as IGH_Goo;
            if (goo == null)
            {
                return item;
            }

            // ScriptVariable is the documented way to get the underlying value out
            // of a Goo without knowing which Goo it is.
            try
            {
                return goo.ScriptVariable() ?? item;
            }
            catch (Exception)
            {
                return item;
            }
        }

        private static PreviewPrimitive Build(object value)
        {
            if (value == null)
            {
                return null;
            }

            // ⚠️ ORDER MATTERS HERE. Mesh before GeometryBase, and the concrete
            // curve types before Curve, because the first match wins and a Circle
            // tested after Curve would be sampled as a generic curve rather than
            // recognised.
            Mesh mesh = value as Mesh;
            if (mesh != null)
            {
                return FromMesh(mesh);
            }

            Brep brep = value as Brep;
            if (brep != null)
            {
                return FromMeshes(Mesh.CreateFromBrep(brep, MeshingParameters.FastRenderMesh));
            }

            Surface surface = value as Surface;
            if (surface != null)
            {
                return FromMeshes(Mesh.CreateFromBrep(surface.ToBrep(), MeshingParameters.FastRenderMesh));
            }

            SubD subd = value as SubD;
            if (subd != null)
            {
                return FromMesh(Mesh.CreateFromSubD(subd, 2));
            }

            Curve curve = value as Curve;
            if (curve != null)
            {
                return FromCurve(curve);
            }

            if (value is Line)
            {
                Line line = (Line)value;
                return FromPoints(new[] { line.From, line.To }, false);
            }

            if (value is Polyline)
            {
                Polyline polyline = (Polyline)value;
                return FromPoints(polyline.ToArray(), polyline.IsClosed);
            }

            if (value is Arc)
            {
                return FromCurve(new ArcCurve((Arc)value));
            }

            if (value is Circle)
            {
                return FromCurve(new ArcCurve((Circle)value));
            }

            if (value is Point3d)
            {
                return FromPoint((Point3d)value);
            }

            Rhino.Geometry.Point point = value as Rhino.Geometry.Point;
            if (point != null)
            {
                return FromPoint(point.Location);
            }

            if (value is Plane)
            {
                return FromPlane((Plane)value);
            }

            if (value is Vector3d)
            {
                // A bare vector has no base, so it is drawn from the origin. A
                // component that means "this vector at this point" should output a
                // Line; guessing an anchor here would be wrong more often than right.
                return FromArrow(Point3d.Origin, (Vector3d)value);
            }

            PointCloud cloud = value as PointCloud;
            if (cloud != null)
            {
                return FromCloud(cloud);
            }

            if (value is BoundingBox)
            {
                return FromBounds((BoundingBox)value);
            }

            if (value is Box)
            {
                return FromBounds(((Box)value).BoundingBox);
            }

            string text = value as string;
            if (text != null)
            {
                return FromText(text);
            }

            return null;
        }

        private static PreviewPrimitive FromMeshes(Mesh[] meshes)
        {
            if (meshes == null || meshes.Length == 0)
            {
                return null;
            }

            Mesh joined = new Mesh();
            foreach (Mesh piece in meshes)
            {
                if (piece != null)
                {
                    joined.Append(piece);
                }
            }

            return FromMesh(joined);
        }

        private static PreviewPrimitive FromMesh(Mesh mesh)
        {
            if (mesh == null || mesh.Vertices.Count == 0)
            {
                return null;
            }

            // Triangulated here rather than host-side: the host draws indices and
            // has no business knowing what a quad was.
            Mesh working = mesh.DuplicateMesh();
            working.Faces.ConvertQuadsToTriangles();
            if (working.Normals.Count != working.Vertices.Count)
            {
                working.Normals.ComputeNormals();
            }

            int vertexCount = working.Vertices.Count;
            double[] positions = new double[vertexCount * 3];
            double[] normals = new double[vertexCount * 3];
            for (int index = 0; index < vertexCount; index++)
            {
                Point3f vertex = working.Vertices[index];
                positions[index * 3] = vertex.X;
                positions[index * 3 + 1] = vertex.Y;
                positions[index * 3 + 2] = vertex.Z;

                if (index < working.Normals.Count)
                {
                    Vector3f normal = working.Normals[index];
                    normals[index * 3] = normal.X;
                    normals[index * 3 + 1] = normal.Y;
                    normals[index * 3 + 2] = normal.Z;
                }
            }

            List<int> indices = new List<int>(working.Faces.Count * 3);
            for (int face = 0; face < working.Faces.Count; face++)
            {
                MeshFace item = working.Faces[face];
                indices.Add(item.A);
                indices.Add(item.B);
                indices.Add(item.C);
            }

            if (indices.Count == 0)
            {
                return null;
            }

            return new PreviewPrimitive
            {
                Kind = PreviewPrimitiveKind.TriangleMesh,
                Positions = positions,
                Normals = normals,
                Indices = indices.ToArray(),
            };
        }

        private static PreviewPrimitive FromCurve(Curve curve)
        {
            if (curve == null)
            {
                return null;
            }

            // A straight line needs two points, not sixty-four. Asking the curve
            // whether it is linear first is what keeps a definition full of lines
            // from costing thirty times its payload.
            if (curve.IsLinear())
            {
                return FromPoints(new[] { curve.PointAtStart, curve.PointAtEnd }, false);
            }

            Polyline polyline;
            if (curve.TryGetPolyline(out polyline))
            {
                return FromPoints(polyline.ToArray(), polyline.IsClosed);
            }

            // ⚠️ SAMPLED ACROSS THE DOMAIN, NOT ALONG THE LENGTH.
            // PointAtNormalizedLength gives prettier spacing and costs an
            // arc-length solve PER SAMPLE, which on a definition full of NURBS is
            // the difference between a preview and a stall. Even spacing is not
            // worth that on a path whose whole performance case is a slider drag.
            Interval domain = curve.Domain;
            Point3d[] samples = new Point3d[CurveSamples + 1];
            for (int index = 0; index <= CurveSamples; index++)
            {
                samples[index] = curve.PointAt(domain.ParameterAt((double)index / CurveSamples));
            }

            return FromPoints(samples, curve.IsClosed);
        }

        private static PreviewPrimitive FromPoints(IList<Point3d> points, bool closed)
        {
            if (points == null || points.Count < 2)
            {
                return null;
            }

            double[] positions = new double[points.Count * 3];
            for (int index = 0; index < points.Count; index++)
            {
                positions[index * 3] = points[index].X;
                positions[index * 3 + 1] = points[index].Y;
                positions[index * 3 + 2] = points[index].Z;
            }

            return new PreviewPrimitive
            {
                Kind = PreviewPrimitiveKind.Polyline3D,
                Positions = positions,
                Closed = closed,
            };
        }

        private static PreviewPrimitive FromPoint(Point3d point)
        {
            return new PreviewPrimitive
            {
                Kind = PreviewPrimitiveKind.PointMarker,
                Positions = new[] { point.X, point.Y, point.Z },
            };
        }

        /// <summary>
        /// Origin plus two axis vectors. The AXES ARE NOT SENT — the host draws
        /// them at constant pixel width from these nine doubles.
        /// </summary>
        private static PreviewPrimitive FromPlane(Plane plane)
        {
            return new PreviewPrimitive
            {
                Kind = PreviewPrimitiveKind.PlaneGizmo,
                Positions = new[]
                {
                    plane.Origin.X, plane.Origin.Y, plane.Origin.Z,
                    plane.XAxis.X, plane.XAxis.Y, plane.XAxis.Z,
                    plane.YAxis.X, plane.YAxis.Y, plane.YAxis.Z,
                },
            };
        }

        /// <summary>Base and tip. The HEAD is built host-side, screen-sized.</summary>
        private static PreviewPrimitive FromArrow(Point3d origin, Vector3d direction)
        {
            Point3d tip = origin + direction;
            return new PreviewPrimitive
            {
                Kind = PreviewPrimitiveKind.Arrow3D,
                Positions = new[] { origin.X, origin.Y, origin.Z, tip.X, tip.Y, tip.Z },
            };
        }

        private static PreviewPrimitive FromCloud(PointCloud cloud)
        {
            if (cloud == null || cloud.Count == 0)
            {
                return null;
            }

            double[] positions = new double[cloud.Count * 3];
            for (int index = 0; index < cloud.Count; index++)
            {
                Point3d location = cloud[index].Location;
                positions[index * 3] = location.X;
                positions[index * 3 + 1] = location.Y;
                positions[index * 3 + 2] = location.Z;
            }

            return new PreviewPrimitive
            {
                Kind = PreviewPrimitiveKind.PointCloud,
                Positions = positions,
            };
        }

        /// <summary>Min and max only. Bounds carries no drawable.</summary>
        private static PreviewPrimitive FromBounds(BoundingBox box)
        {
            if (!box.IsValid)
            {
                return null;
            }

            return new PreviewPrimitive
            {
                Kind = PreviewPrimitiveKind.Bounds,
                Positions = new[] { box.Min.X, box.Min.Y, box.Min.Z, box.Max.X, box.Max.Y, box.Max.Z },
            };
        }

        /// <summary>
        /// Anchored at the origin until a component supplies a point. Layout and
        /// billboarding are the host's, at its camera.
        /// </summary>
        private static PreviewPrimitive FromText(string text)
        {
            if (string.IsNullOrWhiteSpace(text))
            {
                return null;
            }

            return new PreviewPrimitive
            {
                Kind = PreviewPrimitiveKind.BillboardText,
                Positions = new[] { 0.0, 0.0, 0.0 },
                Text = text,
            };
        }
    }
}
