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
        /// <remarks>
        /// <para>
        /// ⚠️ ONE ITEM PRODUCES SEVERAL PRIMITIVES, AND THE SURFACE IS ALWAYS ONE
        /// OF THEM. A Brep is a shaded surface AND the edges that Brep defines;
        /// they are two drawables describing one result, not two alternatives, so
        /// "show edges" ADDS the edges rather than replacing the surface with
        /// them. A viewer that swapped one for the other would be answering a
        /// question nobody asked.
        /// </para>
        /// <para>
        /// ⚠️ THE EDGES COME FROM WHAT THE SOURCE DEFINES, AND ARE NEVER INVENTED.
        /// A Brep DEFINES its edges, so those are sent verbatim. A mesh defines
        /// its FACE edges, so those are -- read off the original faces, BEFORE the
        /// quads are split for the wire. Nothing here guesses a crease angle,
        /// merges coplanar faces or drops a short edge: preview must not invent or
        /// remove geometry it was given, and every one of those would do exactly
        /// that.
        /// </para>
        /// </remarks>
        internal static void Convert(
            object item,
            Guid componentGuid,
            Guid parameterGuid,
            uint branchHash,
            uint itemIndex,
            PreviewSurface surface,
            bool includeEdges,
            List<PreviewPrimitive> into)
        {
            object value = Unwrap(item);
            uint part = 0;

            PreviewPrimitive primitive = Build(value);
            if (primitive != null)
            {
                Stamp(primitive, componentGuid, parameterGuid, branchHash, itemIndex, part++, surface);
                into.Add(primitive);
            }

            if (!includeEdges)
            {
                return;
            }

            foreach (PreviewPrimitive edge in BuildEdges(value))
            {
                Stamp(edge, componentGuid, parameterGuid, branchHash, itemIndex, part++, surface);
                into.Add(edge);
            }
        }

        /// <summary>Whether this item produces anything at all.</summary>
        internal static bool IsSupported(object item)
        {
            return Build(Unwrap(item)) != null;
        }

        private static void Stamp(
            PreviewPrimitive primitive,
            Guid componentGuid,
            Guid parameterGuid,
            uint branchHash,
            uint itemIndex,
            uint part,
            PreviewSurface surface)
        {
            primitive.ComponentGuid = componentGuid;
            primitive.ParameterGuid = parameterGuid;
            primitive.BranchHash = branchHash;
            primitive.ItemIndex = itemIndex;
            // ORed, not assigned: BuildEdges has already marked its primitives
            // as edges, and overwriting that here would lose the one fact the
            // host needs to draw them legibly.
            primitive.Flags |= PreviewFlags.Visible | PreviewFlags.DepthTest;
            // ⚠️ THE SURFACE IS SET BEFORE THE CONTENT HASH, NOT AFTER. It is one
            // of the things Content() covers, so assigning it later would leave a
            // retargeted primitive hashing identical to the one it replaced — the
            // host would never be told, and the geometry would simply stop
            // appearing in the window the author moved it to.
            primitive.Surface = surface;
            primitive.Id = PreviewHash.Identity(componentGuid, parameterGuid, branchHash, itemIndex, part);
            primitive.ContentHash = PreviewHash.Content(primitive);
        }

        /// <summary>
        /// The edges the source itself defines. Empty for anything that defines
        /// none — a curve is already its own line, and a point has no edges.
        /// </summary>
        private static List<PreviewPrimitive> BuildEdges(object value)
        {
            List<PreviewPrimitive> edges = new List<PreviewPrimitive>();
            if (value == null)
            {
                return edges;
            }

            Mesh mesh = value as Mesh;
            if (mesh != null)
            {
                AppendMeshFaceEdges(mesh, edges);
                return edges;
            }

            Brep brep = value as Brep;
            if (brep != null)
            {
                AppendBrepEdges(brep, edges);
                return edges;
            }

            Surface surface = value as Surface;
            if (surface != null)
            {
                AppendBrepEdges(surface.ToBrep(), edges);
                return edges;
            }

            SubD subd = value as SubD;
            if (subd != null)
            {
                // A SubD's own edges, through the mesh it was converted to for
                // display. Same density as the surface primitive, so the two agree.
                AppendMeshFaceEdges(Mesh.CreateFromSubD(subd, 2), edges);
                return edges;
            }

            return edges;
        }

        /// <summary>
        /// ⚠️ A BREP DEFINES ITS EDGES, so they are sent as they are rather than
        /// recovered from the render mesh. The render mesh's boundary is a
        /// tessellation of a trimming curve and is not the same thing: it wobbles
        /// with meshing density, and on a trimmed surface it is visibly not the
        /// trim. This is the whole reason a Brep is not just a mesh.
        /// </summary>
        private static void AppendBrepEdges(Brep brep, List<PreviewPrimitive> edges)
        {
            if (brep == null)
            {
                return;
            }

            foreach (BrepEdge edge in brep.Edges)
            {
                PreviewPrimitive primitive = FromCurve(edge);
                if (primitive != null)
                {
                    primitive.Flags |= PreviewFlags.Edge;
                    edges.Add(primitive);
                }
            }
        }

        /// <summary>
        /// ⚠️ READ OFF THE ORIGINAL FACES, BEFORE THE QUADS ARE SPLIT. FromMesh
        /// triangulates for the wire, and that INVENTS a diagonal across every
        /// quad; taking the edges afterwards would draw those invented diagonals
        /// as though the mesh had them, which is exactly the "wireframe shows all
        /// the triangles" complaint. TopologyEdges is the mesh's own answer to
        /// "what are my edges".
        ///
        /// ⚠️ CHAINED, NOT ONE PRIMITIVE PER EDGE. A modest mesh has thousands of
        /// edges and the batch ceiling is 200000 primitives; one per edge would
        /// spend the whole budget on a single sphere. Chaining walks the same
        /// segments in order — it adds nothing and drops nothing.
        /// </summary>
        private static void AppendMeshFaceEdges(Mesh mesh, List<PreviewPrimitive> edges)
        {
            if (mesh == null || mesh.TopologyEdges.Count == 0)
            {
                return;
            }

            Rhino.Geometry.Collections.MeshTopologyEdgeList edgeList = mesh.TopologyEdges;
            Rhino.Geometry.Collections.MeshTopologyVertexList vertices = mesh.TopologyVertices;

            // Adjacency by topology vertex index, which is EXACT: the mesh already
            // decided which vertices are the same point, so there is no tolerance
            // to get wrong here.
            Dictionary<int, List<int>> neighbours = new Dictionary<int, List<int>>();
            bool[] used = new bool[edgeList.Count];
            for (int index = 0; index < edgeList.Count; index++)
            {
                Rhino.IndexPair pair = edgeList.GetTopologyVertices(index);
                AddNeighbour(neighbours, pair.I, index);
                AddNeighbour(neighbours, pair.J, index);
            }

            for (int start = 0; start < edgeList.Count; start++)
            {
                if (used[start])
                {
                    continue;
                }

                Rhino.IndexPair first = edgeList.GetTopologyVertices(start);
                used[start] = true;
                LinkedList<int> chain = new LinkedList<int>();
                chain.AddLast(first.I);
                chain.AddLast(first.J);

                // Walk forward from the tail, then backward from the head. A chain
                // stops at a junction rather than guessing which way to turn:
                // picking a branch would draw an edge order the mesh never had.
                Walk(edgeList, neighbours, used, chain, true);
                Walk(edgeList, neighbours, used, chain, false);

                double[] positions = new double[chain.Count * 3];
                int cursor = 0;
                foreach (int vertex in chain)
                {
                    Point3f point = vertices[vertex];
                    positions[cursor++] = point.X;
                    positions[cursor++] = point.Y;
                    positions[cursor++] = point.Z;
                }

                edges.Add(new PreviewPrimitive
                {
                    Kind = PreviewPrimitiveKind.Polyline3D,
                    Positions = positions,
                    Closed = false,
                    Flags = PreviewFlags.Edge,
                });
            }
        }

        private static void AddNeighbour(Dictionary<int, List<int>> neighbours, int vertex, int edge)
        {
            List<int> list;
            if (!neighbours.TryGetValue(vertex, out list))
            {
                list = new List<int>(4);
                neighbours[vertex] = list;
            }

            list.Add(edge);
        }

        private static void Walk(
            Rhino.Geometry.Collections.MeshTopologyEdgeList edgeList,
            Dictionary<int, List<int>> neighbours,
            bool[] used,
            LinkedList<int> chain,
            bool forward)
        {
            while (true)
            {
                int end = forward ? chain.Last.Value : chain.First.Value;
                List<int> candidates;
                if (!neighbours.TryGetValue(end, out candidates))
                {
                    return;
                }

                int chosen = -1;
                int unusedCount = 0;
                foreach (int edge in candidates)
                {
                    if (used[edge])
                    {
                        continue;
                    }

                    unusedCount++;
                    chosen = edge;
                }

                // Exactly one way on continues the chain. A junction ends it, so
                // that the segments drawn are the mesh's edges and nothing is
                // implied about how they connect.
                if (unusedCount != 1)
                {
                    return;
                }

                used[chosen] = true;
                Rhino.IndexPair pair = edgeList.GetTopologyVertices(chosen);
                int next = pair.I == end ? pair.J : pair.I;
                if (forward)
                {
                    chain.AddLast(next);
                }
                else
                {
                    chain.AddFirst(next);
                }
            }
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
