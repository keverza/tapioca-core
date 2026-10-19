#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditPathNetwork.hpp"
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

constexpr float kInsetPixels = 16.0f, kGripPixels = 5.0f;
constexpr float kPickPixels = 13.0f; // a grip is caught this far out: well beyond its drawn size

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
    if (QuietPath (s, entity) && s.drag == Drag::None)
        return false;
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
        s.dragStarted = false;
        s.dragReleased = false;
        s.dragPointerPause.reset ();
        s.dragValue = shape.vertices[hot->index].radius;
        s.tracker.Hide (); // locks belong to this grip gesture, never the previous one
        s.cornerEntity = entity;
        s.cornerIndex = hot->index;
        s.cornerSet.clear ();
        for (size_t i : corners)
            s.cornerSet.push_back (shape.vertices[i].id);
        s.startX = mouse.x, s.startY = mouse.y;
        Vec3 press;
        if (m.Onto (mouse.x, mouse.y, shape.vertices[hot->index].p.z, shape.vertices[hot->index].p, press))
            s.navigation.point = press;
        s.navigation.gripInset = Inset (m, shape.vertices[hot->index].p, scale);
        took = true;
    }
    if (!s.navigation.blocked && s.drag == Drag::Corner && ImGui::IsMouseDown (0) && !s.dragStarted)
        s.dragStarted = std::hypot (mouse.x - s.startX, mouse.y - s.startY) >= 3.0f;
    if (s.drag == Drag::Corner && s.dragStarted && ImGui::IsMouseDown (0) && DragPointerMoves (s, mouse)) {
        edit::Entity shape;
        const edit::Entity* before = ShapeOf (s.edits.Before (), s.cornerEntity, shape) ? &shape : nullptr;
        Vec3 at;
        if (before != nullptr && s.cornerIndex < before->vertices.size () &&
            DragOnto (s, m, mouse, before->vertices[s.cornerIndex].p.z, before->vertices[s.cornerIndex].p, at)) {
            std::vector<size_t> set;
            for (edit::Id id : s.cornerSet)
                if (const int i = edit::VertexIndex (*before, id); i >= 0)
                    set.push_back (size_t (i));
            const double inset = s.navigation.gripInset;
            double r = edit::RadiusFromGrip (*before, s.cornerIndex, at, inset);
            r = std::min (std::round (r * 100.0) / 100.0, edit::MaxRadius (*before, set)); // to the centimetre, as fits
            if (const auto locked = s.tracker.Locked (edit::Quantity::Radius))
                r = *locked;
            const edit::Id entity = s.cornerEntity;
            const auto ids = s.cornerSet;
            if (!s.edits.Move (
                    [entity, ids, r] (edit::Document& d, std::string& e) { return SetRadius (d, entity, ids, r, e); }))
                s.message = s.edits.Message ();
            else {
                s.dragValue = r;
                s.navigation.point = at;
            }
            char text[32];
            std::snprintf (text, sizeof text, "R %.2f m", r);
            s.snap = {};
            s.snap.snapped = true;
            s.snap.point = before->vertices[s.cornerIndex].p;
            s.snap.hint = text;
        }
        took = true;
    }
    if (s.drag == Drag::Corner && s.dragStarted && ImGui::IsMouseReleased (0) &&
        s.tracker.State () == edit::TrackerState::Editing)
        s.dragReleased = true;
    if (s.drag == Drag::Corner && ImGui::IsMouseReleased (0) && !s.dragReleased && !s.navigation.blocked) {
        if (!s.dragStarted) {
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

namespace {

const ImU32 kKerb = IM_COL32 (168, 131, 74, 255); // the paved edge's colour

bool Shown (const std::vector<edit::Id>& paths, const edit::PavedCorner& c)
{
    for (edit::Id id : paths)
        if (c.Involves (id))
            return true;
    return false;
}

const std::vector<edit::PavedCorner>& PavedCorners (Session& s)
{
    return PavedSurface (s).corners;
}

// The paths whose kerb grips show: every path paved together with one that has anything of it
// selected -- itself, a control point, a grade break, a range. The paths a junction joins are
// one paved area, and its corners are all filleted from any of them.
std::vector<edit::Id> KerbPaths (Session& s)
{
    std::vector<edit::Id> out;
    if (s.tool != Tool::Select)
        return out;
    for (const auto& r : s.selection.Items ())
        if (edit::FindPath (Doc (s), r.entity) != nullptr &&
            std::find (out.begin (), out.end (), r.entity) == out.end ())
            out.push_back (r.entity);
    const auto& corners = PavedCorners (s);
    for (bool grew = !out.empty (); grew;) {
        grew = false;
        for (const auto& c : corners)
            if (c.key.other != edit::kNoId && Shown (out, c))
                for (edit::Id id : { c.path, c.key.other })
                    if (std::find (out.begin (), out.end (), id) == out.end ())
                        out.push_back (id), grew = true;
    }
    return out;
}

// What a gesture on corner `c` sets to `r`: that corner alone, never its neighbours (the
// path's own radius, for all its corners at once, is the panel's).
edit::Operation KerbEdit (const edit::PavedCorner& c, double r)
{
    return [c, r] (edit::Document& d, std::string& e) { return edit::SetKerbRadius (d, c.path, c.key, r, e); };
}

} // namespace

double SurfaceStep (const Session& s)
{
    return s.edits.InGesture () && Doc (s).revision != s.edits.Before ().revision ? 2.0 : 0.5;
}

const edit::PavedArea& PavedSurface (Session& s)
{
    const double step = SurfaceStep (s);
    if (s.pavedRevision != Doc (s).revision || s.pavedStep != step) {
        s.paved = edit::Pave (Doc (s).paths, step);
        s.pavedRevision = Doc (s).revision;
        s.pavedStep = step;
    }
    return s.paved;
}

const edit::PavedCorner* PickedKerb (Session& s)
{
    if (!s.kerbPicked || !Shown (KerbPaths (s), s.kerb))
        return nullptr;
    for (const auto& c : PavedCorners (s))
        if (c.path == s.kerb.path && c.key.Same (s.kerb.key))
            return &c;
    return nullptr;
}

bool KerbField (const Session& s, double& radius)
{
    if (!s.kerbPicked)
        return false;
    radius = s.kerb.requestedRadius;
    if (s.pavedRevision == Doc (s).revision)
        for (const auto& c : s.paved.corners)
            if (c.path == s.kerb.path && c.key.Same (s.kerb.key))
                radius = c.requestedRadius;
    return true;
}

bool TypeKerb (Session& s, double radius)
{
    const edit::PavedCorner* k = PickedKerb (s);
    if (k == nullptr)
        return false;
    const edit::PavedCorner c = *k;
    if (radius == c.requestedRadius) {
        s.message.clear ();
        return true;
    }
    if (!s.edits.Apply ([c, radius] (edit::Document& d, std::string& e) {
            return edit::SetKerbRadius (d, c.path, c.key, radius, e);
        }))
        s.message = s.edits.Message ();
    else
        s.message.clear ();
    return true;
}

edit::Operation RadiusDragOperation (const Session& s, double radius)
{
    if (!s.dragStarted)
        return {};
    if (s.drag == Drag::Corner)
        return [id = s.cornerEntity, ids = s.cornerSet, radius] (edit::Document& d, std::string& e) {
            return SetRadius (d, id, ids, radius, e);
        };
    if (s.drag == Drag::Kerb)
        return [c = s.kerb, radius] (edit::Document& d, std::string& e) {
            if (radius == c.requestedRadius)
                return true; // candidate is before: discard the preview without adding an override
            return edit::SetKerbRadius (d, c.path, c.key, radius, e);
        };
    return {};
}

bool KerbGrips (Session& s, const Map& m, ImVec2 mouse, bool hovered, float scale)
{
    const std::vector<edit::Id> paths = KerbPaths (s);
    if (s.kerbPicked && !Shown (paths, s.kerb))
        s.kerbPicked = false;
    s.hoverKerb = s.drag == Drag::None ? -1 : s.hoverKerb;
    if (paths.empty () && s.drag != Drag::Kerb)
        return false;
    struct KerbGrip {
        edit::PavedCorner corner;
        Vec3 at; // on its fillet's middle: what the grip is drawn and anchored at
        ImVec2 screen;
    };
    std::vector<KerbGrip> grips;
    for (const auto& c : PavedCorners (s)) {
        if (QuietPath (s, c.path) && s.drag == Drag::None)
            continue;
        KerbGrip g { c, edit::KerbGripAt (c, Inset (m, c.at, scale)), {} };
        if (Shown (paths, c) && m.Screen (g.at, g.screen))
            grips.push_back (g);
    }
    if (s.drag == Drag::None) {
        float best = kPickPixels * scale;
        for (size_t k = 0; hovered && k < grips.size (); ++k) {
            const float d = std::hypot (grips[k].screen.x - mouse.x, grips[k].screen.y - mouse.y);
            if (d < best)
                best = d, s.hoverKerb = int (k);
        }
        if (s.hoverKerb >= 0)
            s.hoverHandle = -1, s.selection.hover = {};
    }
    const KerbGrip* hot = s.drag == Drag::None && s.hoverKerb >= 0 ? &grips[size_t (s.hoverKerb)] : nullptr;
    bool took = false;
    if (hovered && hot != nullptr && (ImGui::IsMouseClicked (0) || ImGui::IsMouseDoubleClicked (0))) {
        if (ImGui::IsMouseDoubleClicked (0)) { // sharp again
            if (!s.edits.Apply (KerbEdit (hot->corner, 0.0)))
                s.message = s.edits.Message ();
            return true;
        }
        if (s.edits.Begin ()) {
            s.drag = Drag::Kerb;
            s.dragStarted = false;
            s.dragReleased = false;
            s.dragPointerPause.reset ();
            s.dragValue = hot->corner.key.radius;
            s.tracker.Hide ();
            s.kerb = hot->corner;
            s.startX = mouse.x, s.startY = mouse.y;
            Vec3 press;
            if (m.Onto (mouse.x, mouse.y, s.kerb.at.z, s.kerb.at, press))
                s.navigation.point = press;
            s.navigation.gripInset = Inset (m, s.kerb.at, scale);
            took = true;
        }
    }
    if (!s.navigation.blocked && s.drag == Drag::Kerb && ImGui::IsMouseDown (0) && !s.dragStarted)
        s.dragStarted = std::hypot (mouse.x - s.startX, mouse.y - s.startY) >= 3.0f;
    if (s.drag == Drag::Kerb && s.dragStarted && ImGui::IsMouseDown (0) && DragPointerMoves (s, mouse)) {
        Vec3 at;
        if (DragOnto (s, m, mouse, s.kerb.at.z, s.kerb.at, at)) {
            double r = edit::KerbRadiusFromGrip (s.kerb, at, s.navigation.gripInset);
            r = std::min (std::round (r * 100.0) / 100.0, s.kerb.maxRadius); // to the centimetre, as fits
            if (const auto locked = s.tracker.Locked (edit::Quantity::Radius))
                r = *locked;
            if (!s.edits.Move (KerbEdit (s.kerb, r)))
                s.message = s.edits.Message ();
            else {
                s.dragValue = r;
                s.navigation.point = at;
            }
            char text[48];
            std::snprintf (text, sizeof text, "Corner R %.2f m", r);
            s.snap = {};
            s.snap.snapped = true;
            s.snap.point = s.kerb.at;
            s.snap.hint = text;
        }
        took = true;
    }
    if (s.drag == Drag::Kerb && s.dragStarted && ImGui::IsMouseReleased (0) &&
        s.tracker.State () == edit::TrackerState::Editing)
        s.dragReleased = true;
    if (s.drag == Drag::Kerb && ImGui::IsMouseReleased (0) && !s.dragReleased && !s.navigation.blocked) {
        if (!s.dragStarted) {
            s.edits.Cancel (); // a click: this corner picked, to be set alone
            s.kerbPicked = true;
        }
        else
            s.edits.End ();
        s.drag = Drag::None;
        s.snap = {};
        took = true;
    }
    // The grips: small diamonds in the kerb's colour, apart from the bends' round ones.
    auto* list = s.canvas.list;
    for (size_t k = 0; k < grips.size (); ++k) {
        const auto& g = grips[k];
        const bool mine =
            (s.kerbPicked || s.drag == Drag::Kerb) && s.kerb.path == g.corner.path && s.kerb.key.Same (g.corner.key);
        const bool lit = int (k) == s.hoverKerb || mine;
        const float r = (lit ? kGripPixels + 2.0f : kGripPixels + 0.5f) * scale;
        const ImVec2 p = g.screen;
        rc::Scope scope (s.canvas, { At (g.at, p) });
        const ImVec2 q[4] = { { p.x, p.y - r }, { p.x + r, p.y }, { p.x, p.y + r }, { p.x - r, p.y } };
        list->AddQuadFilled ({ q[0].x, q[0].y - 1.5f }, { q[1].x + 1.5f, q[1].y }, { q[2].x, q[2].y + 1.5f },
                             { q[3].x - 1.5f, q[3].y }, kShadow);
        list->AddQuadFilled (q[0], q[1], q[2], q[3], lit ? kAccent : kPaper);
        list->AddQuad (q[0], q[1], q[2], q[3], lit ? kInk : kKerb, 1.5f * scale);
    }
    return took;
}

} // namespace geomsrv::archviz::edithud::detail
