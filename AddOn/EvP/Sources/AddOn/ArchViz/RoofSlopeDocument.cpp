#include "ArchViz/RoofSlopeDocument.hpp"
#include <clipper2/clipper.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace geomsrv::archviz::roofslope {
namespace {
namespace cp = Clipper2Lib;
cp::PathD Path (const Ring& ring)
{
    cp::PathD out;
    for (const auto& p : ring)
        out.emplace_back (p.x, p.y);
    return out;
}
// How many other rings contain this ring's first point: even is solid, odd an opening.
std::vector<int> Depths (const std::vector<Ring>& rings)
{
    std::vector<int> depth (rings.size (), 0);
    for (size_t i = 0; i < rings.size (); ++i)
        for (size_t j = 0; j < rings.size (); ++j)
            if (i != j && !rings[i].empty () &&
                cp::PointInPolygon (cp::PointD (rings[i][0].x, rings[i][0].y), Path (rings[j])) ==
                    cp::PointInPolygonResult::IsInside)
                ++depth[i];
    return depth;
}
} // namespace

bool Seed (edit::Document& doc, const Data& data, std::string& error)
{
    if (data.outline.empty () || data.cells.empty ()) {
        error = "Plan the roof before writing it to the editor.";
        return false;
    }
    edit::Document out = doc;
    if (out.roof) {
        const auto old = out.roof->footprints;
        std::erase_if (out.entities, [&] (const edit::Entity& e) {
            return std::find (old.begin (), old.end (), e.id) != old.end ();
        });
        out.roof.reset ();
    }
    edit::RoofDesign roof;
    roof.origin = { data.origin.x, data.origin.y, 0.0 };
    roof.bearing = data.bearing, roof.datumZ = data.topZ, roof.ridgeZ = data.ridgeZ;
    roof.cap = data.cap, roof.targetPercent = data.targetPercent, roof.textHeight = data.textHeight;
    // Each solid ring with the openings directly inside it is one entity.
    const auto depth = Depths (data.outline);
    for (size_t i = 0; i < data.outline.size (); ++i) {
        if (depth[i] % 2 != 0)
            continue;
        std::vector<std::vector<edit::Vec3>> rings (1);
        auto add = [&] (const Ring& ring, std::vector<edit::Vec3>& to) {
            for (const auto& p : ring) {
                const Vec w = World (data, p);
                to.push_back ({ w.x, w.y, data.topZ });
            }
        };
        add (data.outline[i], rings[0]);
        const auto outer = Path (data.outline[i]);
        for (size_t j = 0; j < data.outline.size (); ++j)
            if (depth[j] == depth[i] + 1 && cp::PointInPolygon (cp::PointD (data.outline[j][0].x, data.outline[j][0].y),
                                                                outer) == cp::PointInPolygonResult::IsInside) {
                rings.emplace_back ();
                add (data.outline[j], rings.back ());
            }
        const edit::Id id = edit::AddCompound (out, rings, edit::Kind::Polyline, 0.0, edit::kRoofFootprintRole);
        if (id == edit::kNoId) {
            error = "The roof footprint is not a valid outline with openings inside it.";
            return false;
        }
        roof.footprints.push_back (id);
    }
    std::vector<char> drain (data.nodes.size (), 0), corner (data.nodes.size (), 0);
    for (const auto& c : data.cells) {
        drain[c.drain] = 1;
        for (int n : c.rim)
            corner[n] = 1;
    }
    std::vector<edit::Id> cornerId (data.nodes.size (), edit::kNoId), drainId (data.nodes.size (), edit::kNoId);
    for (size_t i = 0; i < data.nodes.size (); ++i) {
        const Node& n = data.nodes[i];
        const Vec w = World (data, n.xy);
        for (auto [used, kind, ids] : { std::tuple { corner[i], edit::RoofNodeKind::Corner, &cornerId },
                                        std::tuple { drain[i], edit::RoofNodeKind::Drain, &drainId } }) {
            if (!used)
                continue;
            edit::RoofNode node { edit::NewId (out), kind, { w.x, w.y, n.z }, n.lockXY, n.lockZ };
            (*ids)[i] = node.id;
            roof.nodes.push_back (node);
        }
    }
    for (const auto& c : data.cells) {
        edit::RoofCell cell { edit::NewId (out), {}, drainId[c.drain] };
        for (int n : c.rim)
            cell.rim.push_back (cornerId[n]);
        roof.cells.push_back (std::move (cell));
    }
    out.roof = std::move (roof);
    if (!edit::Validate (out, error) || !Check (out, error))
        return false;
    ++out.revision;
    doc = std::move (out);
    return true;
}

bool Read (const edit::Document& doc, Data& data, std::string& error, std::vector<edit::Id>* nodeIds)
{
    if (!doc.roof) {
        error = "This document has no roof.";
        return false;
    }
    const auto& roof = *doc.roof;
    Data d;
    d.origin = { roof.origin.x, roof.origin.y };
    d.bearing = roof.bearing, d.topZ = roof.datumZ, d.ridgeZ = roof.ridgeZ;
    d.cap = roof.cap, d.targetPercent = roof.targetPercent, d.textHeight = roof.textHeight;
    for (edit::Id id : roof.footprints) {
        const edit::Entity* e = edit::Find (doc, id);
        if (e == nullptr) {
            error = "A roof footprint entity is missing.";
            return false;
        }
        // The generator clips against straight segments; a curve would be silently straightened.
        if (e->ellipse || std::any_of (e->vertices.begin (), e->vertices.end (), [] (const edit::Vertex& v) {
                return v.radius != 0.0 || v.in != edit::Vec3 {} || v.out != edit::Vec3 {};
            })) {
            error = "Curved roof footprint edges are not supported yet. Use straight segments.";
            return false;
        }
        for (size_t k = 0; k < edit::ContourCount (*e); ++k) {
            Ring ring;
            for (const auto& p : edit::ContourPoints (*e, k))
                ring.push_back (Local (d, { p.x, p.y }));
            d.outline.push_back (std::move (ring));
        }
    }
    // Plan's convention, whatever winding an edit left: solids counter-clockwise, openings clockwise.
    const auto depth = Depths (d.outline);
    for (size_t i = 0; i < d.outline.size (); ++i)
        if ((cp::Area (Path (d.outline[i])) > 0) != (depth[i] % 2 == 0))
            std::reverse (d.outline[i].begin (), d.outline[i].end ());
    std::map<edit::Id, int> index;
    std::vector<edit::Id> ids;
    for (const auto& n : roof.nodes) {
        index[n.id] = int (d.nodes.size ());
        ids.push_back (n.id);
        d.nodes.push_back ({ Local (d, { n.p.x, n.p.y }), n.p.z, n.lockXY, n.lockZ });
    }
    constexpr double kSquare = 1e-6;
    for (size_t ci = 0; ci < roof.cells.size (); ++ci) {
        const auto& c = roof.cells[ci];
        Cell cell;
        auto at = [&] (edit::Id id) {
            const auto it = index.find (id);
            return it == index.end () ? -1 : it->second;
        };
        for (edit::Id id : c.rim)
            cell.rim.push_back (at (id));
        cell.drain = at (c.drain);
        if (cell.drain < 0 || std::count (cell.rim.begin (), cell.rim.end (), -1) > 0) {
            error = "Roof cell " + std::to_string (ci + 1) + " names a missing node.";
            return false;
        }
        const bool four = cell.rim.size () == 4;
        if (four) {
            const Vec a = d.nodes[cell.rim[0]].xy, b = d.nodes[cell.rim[1]].xy, e = d.nodes[cell.rim[2]].xy,
                      f = d.nodes[cell.rim[3]].xy;
            cell.box = { a.x, a.y, e.x, e.y };
            // Rect order: (u0,v0) (u1,v0) (u1,v1) (u0,v1), axis-aligned in the layout frame.
            if (std::abs (b.x - e.x) > kSquare || std::abs (b.y - a.y) > kSquare || std::abs (f.x - a.x) > kSquare ||
                std::abs (f.y - e.y) > kSquare || !(e.x > a.x) || !(e.y > a.y))
                cell.rim.clear ();
        }
        if (cell.rim.size () != 4) {
            error = "Roof cell " + std::to_string (ci + 1) +
                    " is not a four-corner rectangle in the layout frame; freeform facets are not supported yet.";
            return false;
        }
        d.cells.push_back (std::move (cell));
    }
    if (!Refresh (d, error))
        return false;
    data = std::move (d);
    if (nodeIds)
        *nodeIds = std::move (ids);
    return true;
}

bool Check (const edit::Document& doc, std::string& error)
{
    if (!doc.roof)
        return true;
    Data data;
    return Read (doc, data, error);
}

} // namespace geomsrv::archviz::roofslope
