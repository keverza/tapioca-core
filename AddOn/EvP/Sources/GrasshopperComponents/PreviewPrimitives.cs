using System;
using System.Collections.Generic;

namespace Tapioca.Grasshopper
{
    /// <summary>
    /// The ten preview primitives, and the identity rule that keys the host's
    /// cache. See HANDOFF-GrasshopperInsideArchicad.md, "The primitive set".
    /// </summary>
    /// <remarks>
    /// ⚠️ THESE VALUES ARE A WIRE CONTRACT. The host will mirror them in
    /// Sources/AddOn/Grasshopper/GhPreviewProtocol.hpp, and neither side may infer
    /// the other's — the same rule BridgeProtocol.cs already keeps for the control
    /// protocol. Adding a kind is a version bump on both sides; renumbering one is
    /// a silent corruption that draws the wrong shape.
    /// </remarks>
    internal enum PreviewPrimitiveKind : byte
    {
        TriangleMesh = 1,
        Polyline3D = 2,
        PointMarker = 3,
        PlaneGizmo = 4,
        Arrow3D = 5,
        BillboardText = 6,
        WorldText = 7,
        PointCloud = 8,
        BillboardSprite = 9,
        Bounds = 10,
    }

    /// <summary>
    /// WHERE a primitive is meant to be drawn. Mirrors PreviewSurface in
    /// GhPreviewProtocol.hpp.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ ARCHICAD'S 3D WINDOW AND ITS FLOOR PLAN ARE NOT ONE SURFACE WITH TWO
    /// CAMERAS. They are different host code paths in the add-on already, with
    /// different projections, different units, and different ideas of what a
    /// line's width means. Nothing downstream can tell which a result was meant
    /// for from the geometry alone — a flat polyline at z=0 is a perfectly
    /// ordinary 3D result — so the AUTHOR says, and it crosses the wire.
    /// </para>
    /// <para>
    /// ⚠️ NOT INFERRED FROM THE ACTIVE WINDOW. Binding this to whatever window
    /// happens to be focused would make what the model shows depend on where
    /// someone last clicked. An author who WANTS that can wire Tapir's
    /// GetCurrentWindowType into the component's Target input and see it in the
    /// definition, which is the difference between a choice and hidden state.
    /// </para>
    /// </remarks>
    internal enum PreviewSurface : byte
    {
        Model3D = 1,
        FloorPlan = 2,

        /// <summary>
        /// Drawn in both. The honest answer for geometry that means the same
        /// thing either way — a grid, a site outline, a setting-out point.
        /// </summary>
        Both = 3,
    }

    [Flags]
    internal enum PreviewFlags : byte
    {
        None = 0,
        Visible = 1 << 0,
        Selected = 1 << 1,
        Highlighted = 1 << 2,
        XRay = 1 << 3,
        DepthTest = 1 << 4,

        /// <summary>
        /// This primitive is an EDGE OF ANOTHER one in the same batch, rather
        /// than something the author wired directly.
        /// </summary>
        /// <remarks>
        /// ⚠️ IT IS INFORMATION, NOT DECORATION. Edges and curves are both
        /// Polyline3D on the wire, and the host cannot tell them apart from the
        /// geometry: an edge drawn in the surface's own colour, over that
        /// surface, is invisible -- which would make "show edges" appear to do
        /// nothing. The host needs to know which is which to draw either one
        /// legibly, and later to style them separately.
        /// </remarks>
        Edge = 1 << 5,
    }

    /// <summary>
    /// One captured primitive, before it is framed for the wire.
    /// </summary>
    /// <remarks>
    /// <para>
    /// ⚠️ IDENTITY IS NOT POSITION IN A LIST, AND IT IS NOT CONTENT.
    /// <see cref="Id"/> is derived from where the primitive came from — which
    /// component, which output parameter, which branch, which item — so that
    /// editing a slider changes a primitive's CONTENT while leaving its IDENTITY
    /// alone. If the id depended on content, every edit would read to the host as
    /// a Remove plus an Add, the delta protocol would degenerate into full
    /// retransmission, and selection state would be lost on every solve.
    /// </para>
    /// <para>
    /// <see cref="ContentHash"/> is the other half: it is what "Changed" is
    /// decided by, and it must cover everything the host draws, or an edit that
    /// moves a vertex without changing a count would never be sent.
    /// </para>
    /// </remarks>
    internal sealed class PreviewPrimitive
    {
        internal ulong Id;

        internal PreviewPrimitiveKind Kind;

        internal PreviewFlags Flags;

        /// <summary>
        /// Which Archicad window this is for. Part of the CONTENT, not the
        /// identity: an author who retargets a component from the 3D window to
        /// the floor plan has changed what it produces, not produced something
        /// else, so it must diff as Changed and keep its selection.
        /// </summary>
        internal PreviewSurface Surface = PreviewSurface.Model3D;

        internal uint ItemIndex;

        internal Guid ComponentGuid;

        internal Guid ParameterGuid;

        internal uint BranchHash;

        internal ulong ContentHash;

        /// <summary>
        /// Positions, and for a mesh the normals and indices. Kept as flat arrays
        /// because that is the shape the wire and the GPU both want; nothing here
        /// is a Rhino type, so this class is safe to name anywhere in the package.
        /// </summary>
        internal double[] Positions;

        internal double[] Normals;

        internal int[] Indices;

        /// <summary>UTF-8 payload for the text primitives; null otherwise.</summary>
        internal string Text;

        /// <summary>Closed flag for Polyline3D; ignored by other kinds.</summary>
        internal bool Closed;

        internal int VertexCount
        {
            get { return Positions == null ? 0 : Positions.Length / 3; }
        }
    }

    /// <summary>
    /// Identity and content hashing, kept together because they are the two
    /// halves of one rule and getting either wrong breaks the delta protocol.
    /// </summary>
    /// <remarks>
    /// FNV-1a 64. Chosen because it is trivial to reimplement byte-for-byte in
    /// C++ on the host side — which is a requirement, not a preference: the host
    /// recomputes nothing, but a diagnostic that cannot reproduce an id cannot
    /// explain a cache miss. A cryptographic hash would buy nothing here; nothing
    /// about preview identity is a security boundary.
    /// </remarks>
    internal static class PreviewHash
    {
        private const ulong Offset = 14695981039346656037UL;
        private const ulong Prime = 1099511628211UL;

        internal static ulong Start()
        {
            return Offset;
        }

        internal static ulong Byte(ulong hash, byte value)
        {
            return (hash ^ value) * Prime;
        }

        internal static ulong UInt32(ulong hash, uint value)
        {
            hash = Byte(hash, (byte)(value & 0xFF));
            hash = Byte(hash, (byte)((value >> 8) & 0xFF));
            hash = Byte(hash, (byte)((value >> 16) & 0xFF));
            return Byte(hash, (byte)((value >> 24) & 0xFF));
        }

        internal static ulong UInt64(ulong hash, ulong value)
        {
            hash = UInt32(hash, (uint)(value & 0xFFFFFFFFUL));
            return UInt32(hash, (uint)((value >> 32) & 0xFFFFFFFFUL));
        }

        internal static ulong Guid(ulong hash, Guid value)
        {
            foreach (byte item in value.ToByteArray())
            {
                hash = Byte(hash, item);
            }

            return hash;
        }

        /// <summary>
        /// ⚠️ ROUNDED BEFORE HASHING, DELIBERATELY. A double that differs in its
        /// last bit is the same preview, and hashing the raw bits would report a
        /// Changed on every solve for geometry that merely went through a
        /// different arithmetic order. The tolerance is a PREVIEW tolerance and is
        /// far coarser than anything BIM; it exists to stop spurious retransmission.
        /// </summary>
        internal static ulong Double(ulong hash, double value)
        {
            long quantised = (long)Math.Round(value * 1e6);
            return UInt64(hash, unchecked((ulong)quantised));
        }

        internal static ulong Doubles(ulong hash, double[] values)
        {
            if (values == null)
            {
                return UInt32(hash, 0u);
            }

            hash = UInt32(hash, (uint)values.Length);
            foreach (double value in values)
            {
                hash = Double(hash, value);
            }

            return hash;
        }

        internal static ulong Ints(ulong hash, int[] values)
        {
            if (values == null)
            {
                return UInt32(hash, 0u);
            }

            hash = UInt32(hash, (uint)values.Length);
            foreach (int value in values)
            {
                hash = UInt32(hash, unchecked((uint)value));
            }

            return hash;
        }

        internal static ulong Text(ulong hash, string value)
        {
            if (string.IsNullOrEmpty(value))
            {
                return UInt32(hash, 0u);
            }

            byte[] bytes = System.Text.Encoding.UTF8.GetBytes(value);
            hash = UInt32(hash, (uint)bytes.Length);
            foreach (byte item in bytes)
            {
                hash = Byte(hash, item);
            }

            return hash;
        }

        /// <summary>
        /// The cache key: where the primitive came from, never what it contains.
        /// </summary>
        /// <summary>
        /// The cache key for one primitive.
        /// </summary>
        /// <remarks>
        /// ⚠️ <paramref name="part"/> EXISTS BECAUSE ONE WIRED ITEM PRODUCES
        /// SEVERAL PRIMITIVES. A Brep is a shaded surface AND the edges that Brep
        /// defines; without a part number every one of them would hash to the
        /// same id, and the cache would keep only whichever arrived last -- the
        /// surface would replace its own edges on every solve, or the other way
        /// round, and the delta protocol would report a Change where nothing
        /// changed.
        ///
        /// ⚠️ AND IT MUST BE POSITIONAL, NOT CONTENT-DERIVED, for the reason the
        /// whole identity rule exists: an edge that MOVES has to stay the same
        /// primitive, or an edit reads as a Remove plus an Add and selection is
        /// lost every solve.
        /// </remarks>
        internal static ulong Identity(
            Guid componentGuid, Guid parameterGuid, uint branchHash, uint itemIndex, uint part)
        {
            ulong hash = Start();
            hash = Guid(hash, componentGuid);
            hash = Guid(hash, parameterGuid);
            hash = UInt32(hash, branchHash);
            hash = UInt32(hash, itemIndex);
            return UInt32(hash, part);
        }

        /// <summary>
        /// What "Changed" is decided by: everything the host draws, and the kind,
        /// so that a point becoming a plane is a change rather than a coincidence.
        /// </summary>
        internal static ulong Content(PreviewPrimitive primitive)
        {
            ulong hash = Start();
            hash = Byte(hash, (byte)primitive.Kind);
            // Retargeting a component is a content change, so it is hashed here
            // rather than in Identity. Leaving it out would let a definition
            // switch from the 3D window to the floor plan without the host ever
            // being told, and the geometry would simply stop appearing.
            hash = Byte(hash, (byte)primitive.Surface);
            hash = Byte(hash, primitive.Closed ? (byte)1 : (byte)0);
            hash = Doubles(hash, primitive.Positions);
            hash = Doubles(hash, primitive.Normals);
            hash = Ints(hash, primitive.Indices);
            return Text(hash, primitive.Text);
        }

        /// <summary>
        /// A GH_Path hashed to fit the fixed-size header. The path TEXT stays
        /// worker-side: the host does not need it to draw, only to answer "what am
        /// I looking at", and that is an off-hot-path request.
        /// </summary>
        internal static uint Branch(IEnumerable<int> path)
        {
            ulong hash = Start();
            if (path != null)
            {
                foreach (int index in path)
                {
                    hash = UInt32(hash, unchecked((uint)index));
                }
            }

            return (uint)(hash ^ (hash >> 32));
        }
    }
}
