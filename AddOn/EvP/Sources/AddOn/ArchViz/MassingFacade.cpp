#include "ArchViz/MassingSlices.hpp"
#include "Geometry/GeometryEngine.hpp"
#include "Geometry/SliceEngine.hpp"
#include <clipper2/clipper.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace geomsrv::archviz::massingslices {
namespace {
namespace cp = Clipper2Lib;
using V = engine::Vector3;
constexpr double kCos70 = 0.3420201433256687;
constexpr double kPlaneTolerance = 1e-7;
V Sub (const V& a, const V& b)
{
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}
V Scale (const V& v, double s)
{
    return { v.x * s, v.y * s, v.z * s };
}
V Point (const Mesh& m, size_t i)
{
    return { m.vertices[i * 3], m.vertices[i * 3 + 1], m.vertices[i * 3 + 2] };
}
struct Face {
    V p[3], normal;
    size_t body = 0;
};
bool Fail (std::string& error, const char* text)
{
    error = text;
    return false;
}

bool Footprints (const std::vector<Input>& inputs, std::vector<cp::PathsD>& footprints, double ox, double oy,
                 std::string& error)
{
    size_t points = 0;
    for (const auto& input : inputs) {
        if (!std::isfinite (input.slab.bottom) || !std::isfinite (input.slab.top) ||
            input.slab.top <= input.slab.bottom)
            return Fail (error, "Invalid facade prism height.");
        cp::PathsD rings;
        bool valid = true;
        const auto add = [&] (const slabslices::Ring& ring) {
            if (ring.xy.size () < 6 || ring.xy.size () % 2 || ring.xy.size () > 40000) {
                valid = false;
                return;
            }
            for (double coordinate : ring.xy)
                valid &= std::isfinite (coordinate) && std::abs (coordinate) <= 1e9;
            if (!valid)
                return;
            const auto contour = slabslices::Contour (ring, 0.05);
            cp::PathD path;
            for (size_t i = 0; i + 1 < contour.xy.size (); i += 2)
                path.emplace_back (contour.xy[i] - ox, contour.xy[i + 1] - oy);
            points += path.size ();
            rings.push_back (std::move (path));
        };
        add (input.slab.outer);
        for (const auto& hole : input.slab.holes)
            add (hole);
        if (!valid)
            return Fail (error, "Invalid facade prism contour.");
        if (points > 20000)
            return Fail (error, "Facade footprint budget exceeded.");
        footprints.push_back (cp::Union (rings, cp::FillRule::EvenOdd, 7));
    }
    return true;
}

// Preserve the exact band/perimeter shortcut when no operated/sloping surface is supplied.
bool Prisms (const std::vector<Input>& inputs, double& area, std::string& error)
{
    std::vector<cp::PathsD> footprints;
    if (!Footprints (inputs, footprints, inputs.front ().slab.outer.xy[0], inputs.front ().slab.outer.xy[1], error))
        return false;
    std::vector<double> levels;
    for (const auto& input : inputs)
        levels.insert (levels.end (), { input.slab.bottom, input.slab.top });
    std::sort (levels.begin (), levels.end ());
    levels.erase (std::unique (levels.begin (), levels.end ()), levels.end ());
    double total = 0;
    size_t work = 0;
    for (size_t k = 1; k < levels.size (); ++k) {
        const double z = (levels[k - 1] + levels[k]) / 2;
        cp::PathsD active;
        for (size_t i = 0; i < inputs.size (); ++i)
            if (inputs[i].slab.bottom < z && inputs[i].slab.top > z)
                for (const auto& ring : footprints[i]) {
                    work += ring.size ();
                    if (work > 1000000)
                        return Fail (error, "Facade union work budget exceeded.");
                    active.push_back (ring);
                }
        for (const auto& ring : cp::Union (active, cp::FillRule::NonZero, 7))
            for (size_t i = 0; i < ring.size (); ++i) {
                const auto& a = ring[i];
                const auto& b = ring[(i + 1) % ring.size ()];
                total += std::hypot (b.x - a.x, b.y - a.y) * (levels[k] - levels[k - 1]);
            }
    }
    area = total;
    return true;
}

Mesh Prism (const Input& input, const cp::PathsD& paths, double ox, double oy)
{
    Mesh mesh;
    std::vector<SliceChain> chains;
    const auto vertex = [&] (double x, double y, double z) {
        mesh.triangles.push_back (uint32_t (mesh.VertexCount ()));
        mesh.vertices.insert (mesh.vertices.end (), { x + ox, y + oy, z });
    };
    for (const auto& path : paths) {
        SliceChain chain;
        chain.closed = true;
        for (const auto& p : path)
            chain.xy.insert (chain.xy.end (), { p.x, p.y });
        chains.push_back (std::move (chain));
        for (size_t i = 0; i < path.size (); ++i) {
            const auto& a = path[i];
            const auto& b = path[(i + 1) % path.size ()];
            vertex (a.x, a.y, input.slab.bottom);
            vertex (b.x, b.y, input.slab.bottom);
            vertex (b.x, b.y, input.slab.top);
            vertex (a.x, a.y, input.slab.bottom);
            vertex (b.x, b.y, input.slab.top);
            vertex (a.x, a.y, input.slab.top);
        }
    }
    std::vector<StorySliceFillVertex> caps;
    BuildSliceFill (chains, 0, caps);
    for (size_t i = 0; i + 2 < caps.size (); i += 3) {
        const double cross = (caps[i + 1].x - caps[i].x) * (caps[i + 2].y - caps[i].y) -
                             (caps[i + 1].y - caps[i].y) * (caps[i + 2].x - caps[i].x);
        for (int j : { 0, cross > 0 ? 1 : 2, cross > 0 ? 2 : 1 })
            vertex (caps[i + j].x, caps[i + j].y, input.slab.top);
        for (int j : { 0, cross > 0 ? 2 : 1, cross > 0 ? 1 : 2 })
            vertex (caps[i + j].x, caps[i + j].y, input.slab.bottom);
    }
    return mesh;
}

bool Surfaces (const std::vector<std::shared_ptr<const Mesh>>& bodies, double& area, std::string& error)
{
    std::vector<Face> faces;
    size_t triangles = 0;
    for (size_t b = 0; b < bodies.size (); ++b) {
        const auto& body = *bodies[b];
        triangles += body.TriangleCount ();
        if (body.vertices.empty () || body.vertices.size () > 600000 || body.triangles.empty () ||
            body.vertices.size () % 3 || body.triangles.size () % 3 || triangles > 100000)
            return Fail (error, "Invalid or over-budget facade body.");
        for (double value : body.vertices)
            if (!std::isfinite (value) || std::abs (value) > 1e9)
                return Fail (error, "Invalid facade body coordinate.");
        for (size_t i = 0; i < body.triangles.size (); i += 3) {
            Face f;
            f.body = b;
            for (size_t k = 0; k < 3; ++k) {
                if (body.triangles[i + k] >= body.VertexCount ())
                    return Fail (error, "Invalid facade body index.");
                f.p[k] = Point (body, body.triangles[i + k]);
            }
            const V cross = engine::Cross (Sub (f.p[1], f.p[0]), Sub (f.p[2], f.p[0]));
            const double length = std::sqrt (engine::Dot (cross, cross));
            if (length < 1e-12)
                continue;
            f.normal = Scale (cross, 1 / length);
            if (std::abs (f.normal.z) <= kCos70 + 1e-12)
                faces.push_back (f);
        }
    }
    double total = 0;
    size_t work = 0;
    const auto began = std::chrono::steady_clock::now ();
    for (size_t i = 0; i < faces.size (); ++i) {
        if (std::chrono::steady_clock::now () - began > std::chrono::seconds (2))
            return Fail (error, "Facade surface work time budget exceeded.");
        const auto& face = faces[i];
        const V origin = face.p[0], n = face.normal;
        const V edge = Sub (face.p[1], origin);
        const V u = Scale (edge, 1 / std::sqrt (engine::Dot (edge, edge))), v = engine::Cross (n, u);
        const auto project = [&] (const V& p) {
            const auto d = Sub (p, origin);
            return cp::PointD (engine::Dot (d, u), engine::Dot (d, v));
        };
        const cp::PathD subject { project (face.p[0]), project (face.p[1]), project (face.p[2]) };
        cp::PathsD covered;
        for (size_t b = 0; b < bodies.size (); ++b) {
            if (b == face.body)
                continue; // completed operated body already has its internal surfaces removed
            const auto& body = *bodies[b];
            work += body.TriangleCount ();
            if (work > 2000000 || std::chrono::steady_clock::now () - began > std::chrono::seconds (2))
                return Fail (error, "Facade exposed-surface intersection budget exceeded.");
            std::vector<double> local;
            local.reserve (body.vertices.size ());
            double low = 1e100, high = -1e100;
            for (size_t k = 0; k < body.VertexCount (); ++k) {
                const V d = Sub (Point (body, k), origin);
                double z = engine::Dot (d, n);
                // Exterior-side limit: coplanar vertices belong below the plane.
                // Thus opposite shared walls cover the face, same-facing walls do not.
                if (std::abs (z) <= kPlaneTolerance)
                    z = -kPlaneTolerance;
                low = (std::min) (low, z);
                high = (std::max) (high, z);
                local.insert (local.end (), { engine::Dot (d, u), engine::Dot (d, v), z });
            }
            if (low >= 0 || high <= 0)
                continue;
            cp::PathsD section;
            for (const auto& loop : SliceMesh (local.data (), body.VertexCount (), body.triangles.data (),
                                               body.TriangleCount (), 0, 1e-7)) {
                if (!loop.closed)
                    return Fail (error, "Open facade occluder cut; no partial area reported.");
                cp::PathD path;
                for (size_t k = 0; k + 1 < loop.PointCount (); ++k)
                    path.emplace_back (loop.pts[k * 3], loop.pts[k * 3 + 1]);
                if (path.size () >= 3)
                    section.push_back (std::move (path));
            }
            const auto normalized = cp::Union (section, cp::FillRule::EvenOdd, 7);
            covered.insert (covered.end (), normalized.begin (), normalized.end ());
        }
        // Same-facing coincident surfaces are owned once, irrespective of triangulation.
        for (size_t j = 0; j < i; ++j) {
            if (++work > 2000000)
                return Fail (error, "Facade coplanar union budget exceeded.");
            const auto& other = faces[j];
            if (engine::Dot (other.normal, n) < 1 - 1e-10)
                continue;
            bool coplanar = true;
            for (const auto& p : other.p)
                coplanar &= std::abs (engine::Dot (Sub (p, origin), n)) <= kPlaneTolerance;
            if (coplanar)
                covered.push_back ({ project (other.p[0]), project (other.p[1]), project (other.p[2]) });
        }
        const auto clip = cp::Union (covered, cp::FillRule::NonZero, 7);
        total += std::abs (cp::Area (cp::Difference (cp::PathsD { subject }, clip, cp::FillRule::NonZero, 7)));
    }
    if (!std::isfinite (total))
        return Fail (error, "Non-finite facade area.");
    area = total;
    return true;
}
} // namespace

bool Facade (const std::vector<Input>& inputs, double& area, std::string& error)
{
    error.clear ();
    if (inputs.size () > 128)
        return Fail (error, "Facade source budget exceeded.");
    if (inputs.empty ()) {
        area = 0;
        return true;
    }
    bool surfaces = false;
    for (const auto& input : inputs) {
        surfaces |= bool (input.body || input.facadeBody);
        if (!input.body && !input.facadeBody && (input.slab.outer.xy.size () < 6 || input.slab.slopedEdges))
            return Fail (error, "Facade awaits current sloped slab body; no prism substitute.");
    }
    if (!surfaces)
        return Prisms (inputs, area, error);
    std::vector<std::shared_ptr<const Mesh>> bodies;
    for (const auto& input : inputs) {
        // SEO body always wins over a separate surface/prism representation.
        const auto body = input.body ? input.body : input.facadeBody;
        if (body)
            bodies.push_back (body);
        else {
            std::vector<cp::PathsD> footprints;
            const double ox = input.slab.outer.xy[0], oy = input.slab.outer.xy[1];
            if (!Footprints ({ input }, footprints, ox, oy, error))
                return false;
            bodies.push_back (std::make_shared<const Mesh> (Prism (input, footprints.front (), ox, oy)));
        }
    }
    return Surfaces (bodies, area, error);
}
} // namespace geomsrv::archviz::massingslices
