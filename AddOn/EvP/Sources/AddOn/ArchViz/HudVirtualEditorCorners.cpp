#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditShape.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

// Live-corner grips on the locked view: a small circle on each corner's bisector, at its
// arc's middle or just inside a sharp corner. Dragging one along the bisector sets the
// radius -- every corner of an object selected whole, or the selected corners -- clamped
// to what fits; a click picks the corner (its radius can then be typed); a double click
// makes it sharp again. A path's bends are the same: its centreline is lines and arcs, a
// control point's radius the arc's (EditPathEdit Centreline).
namespace geomsrv::archviz::edithud::detail {

using edit::Vec3;
namespace rc = roofcanvas;

namespace {

constexpr float kInsetPixels = 16.0f, kGripPixels = 5.0f, kPickPixels = 8.0f;

struct Grip {
    edit::Id entity = edit::kNoId;
    size_t index = 0;
    Vec3 at;
    ImVec2 screen;
};

// The outline whose corners are rounded: an object's own, or a path's centreline.
bool ShapeOf (const edit::Document& doc, edit::Id id, edit::Entity& shape)
{
    if (const edit::Path* p = edit::FindPath (doc, id)) {
        shape = edit::Centreline (*p);
        return true;
    }
    if (const edit::Entity* e = edit::Find (doc, id)) {
        shape = *e;
        return true;
    }
    return false;
}

// Sets the radius of corners `ids` of `id`: an object's live corners or a path's bends.
bool SetRadius (edit::Document& d, edit::Id id, const std::vector<edit::Id>& ids, double r, std::string& e)
{
    return edit::FindPath (d, id) != nullptr ? edit::SetPathRadii (d, id, ids, r, e)
                                             : edit::SetCornerRadius (d, id, ids, r, e);
}

// The corners whose grips show: all of an object or a path selected whole, else the selected
// corners (vertices, a path's control points) of one of them.
bool Corners (const Session& s, edit::Id& entity, edit::Entity& shape, std::vector<size_t>& corners)
{
    corners.clear ();
    const auto& items = s.selection.Items ();
    if (items.empty () || s.tool != Tool::Select)
        return false;
    entity = items.front ().entity;
    if (!ShapeOf (Doc (s), entity, shape))
        return false;
    for (const auto& r : items) {
        if (r.entity != entity)
            return false;
        for (size_t i = 0; i < shape.vertices.size (); ++i) {
            edit::CornerGeometry c;
            const bool picked =
                (r.part == edit::Part::Vertex || r.part == edit::Part::PathPoint) && shape.vertices[i].id == r.item;
            const bool wanted = r.part == edit::Part::Entity || r.part == edit::Part::Face || picked;
            if (wanted && edit::CornerOf (shape, i, c) &&
                std::find (corners.begin (), corners.end (), i) == corners.end ())
                corners.push_back (i);
        }
    }
    return !corners.empty ();
}

double Inset (const Map& m, Vec3 at, float scale)
{
    ImVec2 q;
    if (!m.Screen (at, q))
        return 0.5;
    return kInsetPixels * scale * m.MetresPerPixel (q.x, q.y, at.z, at);
}

std::vector<Grip> Grips (const Session& s, const Map& m, float scale)
{
    std::vector<Grip> out;
    edit::Id entity = edit::kNoId;
    edit::Entity e;
    std::vector<size_t> corners;
    if (!Corners (s, entity, e, corners))
        return out;
    for (size_t i : corners) {
        Grip g { entity, i, edit::CornerGripAt (e, i, Inset (m, e.vertices[i].p, scale)), {} };
        if (m.Screen (g.at, g.screen))
            out.push_back (g);
    }
    return out;
}

} // namespace

bool CornerGrips (Session& s, const Map& m, ImVec2 mouse, bool hovered, float scale)
{
    const auto grips = Grips (s, m, scale);
    // Hover: a grip is above the gizmo and the geometry.
    if (s.drag == Drag::None) {
        s.hoverCorner = -1;
        float best = kPickPixels * scale;
        for (size_t k = 0; hovered && k < grips.size (); ++k) {
            const float d = std::hypot (grips[k].screen.x - mouse.x, grips[k].screen.y - mouse.y);
            if (d < best)
                best = d, s.hoverCorner = int (k);
        }
        if (s.hoverCorner >= 0)
            s.hoverHandle = -1, s.selection.hover = {};
    }
    bool took = false;
    const Grip* hot =
        s.hoverCorner >= 0 && size_t (s.hoverCorner) < grips.size () ? &grips[size_t (s.hoverCorner)] : nullptr;
    // A double click on a grip makes its corners sharp.
    if (hovered && hot != nullptr && s.drag == Drag::None && ImGui::IsMouseDoubleClicked (0)) {
        edit::Id entity = edit::kNoId;
        edit::Entity shape;
        std::vector<size_t> corners;
        Corners (s, entity, shape, corners);
        std::vector<edit::Id> ids;
        for (size_t i : corners)
            ids.push_back (shape.vertices[i].id);
        if (!s.edits.Apply (
                [entity, ids] (edit::Document& d, std::string& e) { return SetRadius (d, entity, ids, 0.0, e); }))
            s.message = s.edits.Message ();
        return true;
    }
    // Press: the corners being set are those whose grips show.
    if (hovered && hot != nullptr && s.drag == Drag::None && ImGui::IsMouseClicked (0) && s.edits.Begin ()) {
        edit::Id entity = edit::kNoId;
        edit::Entity shape;
        std::vector<size_t> corners;
        Corners (s, entity, shape, corners);
        s.drag = Drag::Corner;
        s.cornerEntity = entity;
        s.cornerIndex = hot->index;
        s.cornerSet.clear ();
        for (size_t i : corners)
            s.cornerSet.push_back (shape.vertices[i].id);
        s.startX = mouse.x, s.startY = mouse.y;
        took = true;
    }
    if (s.drag == Drag::Corner && ImGui::IsMouseDown (0)) {
        edit::Entity shape;
        const edit::Entity* before = ShapeOf (s.edits.Before (), s.cornerEntity, shape) ? &shape : nullptr;
        Vec3 at;
        if (before != nullptr && s.cornerIndex < before->vertices.size () &&
            m.Onto (mouse.x, mouse.y, before->vertices[s.cornerIndex].p.z, before->vertices[s.cornerIndex].p, at)) {
            std::vector<size_t> set;
            for (edit::Id id : s.cornerSet)
                if (const int i = edit::VertexIndex (*before, id); i >= 0)
                    set.push_back (size_t (i));
            const double inset = Inset (m, before->vertices[s.cornerIndex].p, scale);
            double r = edit::RadiusFromGrip (*before, s.cornerIndex, at, inset);
            r = std::min (std::round (r * 100.0) / 100.0, edit::MaxRadius (*before, set)); // to the centimetre, as fits
            const edit::Id entity = s.cornerEntity;
            const auto ids = s.cornerSet;
            if (!s.edits.Move (
                    [entity, ids, r] (edit::Document& d, std::string& e) { return SetRadius (d, entity, ids, r, e); }))
                s.message = s.edits.Message ();
            char text[32];
            std::snprintf (text, sizeof text, "R %.2f m", r);
            s.snap = {};
            s.snap.snapped = true;
            s.snap.point = before->vertices[s.cornerIndex].p;
            s.snap.hint = text;
        }
        took = true;
    }
    if (s.drag == Drag::Corner && ImGui::IsMouseReleased (0)) {
        if (std::hypot (mouse.x - s.startX, mouse.y - s.startY) < 3.0f) {
            // A click: the corner is picked, its radius ready to type.
            s.edits.Cancel ();
            edit::Entity shape;
            if (ShapeOf (Doc (s), s.cornerEntity, shape) && s.cornerIndex < shape.vertices.size ())
                s.selection.Set (
                    { { s.cornerEntity,
                        edit::FindPath (Doc (s), s.cornerEntity) ? edit::Part::PathPoint : edit::Part::Vertex,
                        shape.vertices[s.cornerIndex].id } });
        }
        else
            s.edits.End ();
        s.drag = Drag::None;
        s.snap = {};
        took = true;
    }
    // The grips, over the geometry.
    auto* list = s.canvas.list;
    for (size_t k = 0; k < grips.size (); ++k) {
        const Grip& g = grips[k];
        const bool lit = int (k) == s.hoverCorner || (s.drag == Drag::Corner && g.index == s.cornerIndex);
        const float r = (lit ? kGripPixels + 1.5f : kGripPixels) * scale;
        rc::Scope scope (s.canvas, { At (g.at, g.screen) });
        list->AddCircleFilled (g.screen, r + 1.5f, kShadow);
        list->AddCircleFilled (g.screen, r, lit ? kAccent : kPaper);
        list->AddCircle (g.screen, r, lit ? kInk : kAccent, 0, 1.5f * scale);
        list->AddCircleFilled (g.screen, 1.5f * scale, lit ? kPaper : kAccent);
    }
    return took;
}

} // namespace geomsrv::archviz::edithud::detail
