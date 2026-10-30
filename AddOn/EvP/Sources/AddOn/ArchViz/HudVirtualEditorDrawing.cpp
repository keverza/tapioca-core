#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditArrange.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditShape.hpp"
#include <cmath>
#include <algorithm>

namespace geomsrv::archviz::edithud::detail {

using edit::Vec3;

// Ends a gumball drag at what it shows now: one undo entry when it changed anything.
void EndHandleDrag (Session& s)
{
    s.edits.End ();
    s.message = s.edits.Message ();
    s.selection.Prune (Doc (s));
    s.snap = {};
    // A dragged pivot stays where it was put; one already set travels with its selection.
    if (s.message.empty () && s.handle >= 0 &&
        (s.dragTransform.relocate || (s.pivot && s.pivotFor == s.selection.Items ()))) {
        s.pivot = edit::Moved (s.frame, s.dragTransform).origin;
        s.pivotFor = s.selection.Items ();
    }
    s.facePush = false;
    s.drag = Drag::None;
    s.dragReleased = false;
    CancelGumballReposition (s);
}

void EndDrag (Session& s)
{
    if (s.drag == Drag::Handle)
        EndHandleDrag (s);
    else {
        s.edits.End ();
        s.drag = Drag::None;
        s.dragReleased = false;
        s.snap = {};
    }
    s.dragPointerPause.reset ();
    s.selection.Prune (Doc (s));
    s.message = s.edits.Message ();
}

namespace {
// In a landscape a drawn line is a source: a path always its centreline, anything else with the
// panel's role. A path can never be the property line, which is a closed ring.
bool RegisterSource (edit::Document& d, edit::Id created, bool path, tapioca::geometry::SourceRole role,
                     std::string& error)
{
    if (!d.landscape)
        return true;
    if (!path)
        return tapioca::geometry::SetLandscapeSourceRole (d, created, role, error);
    tapioca::geometry::LandscapeSource source;
    source.pathId = created;
    source.role = tapioca::geometry::SourceRole::PathCenterline;
    edit::Id id = edit::kNoId;
    return tapioca::geometry::AddLandscapeSource (d, source, id, error);
}
} // namespace

edit::PrimitiveInput ShapeInput (const Session& s, edit::Vec3 second, bool uniform)
{
    edit::PrimitiveInput in;
    in.kind = s.tool == Tool::Rectangle ? edit::Primitive::Rectangle
              : s.tool == Tool::Polygon ? edit::Primitive::Polygon
                                        : edit::Primitive::Ellipse;
    in.first = s.drawing.empty () ? second : s.drawing.front ();
    in.second = second;
    in.u = s.plane.u;
    in.v = s.plane.v;
    in.sides = s.polygonSides;
    in.uniform = uniform;
    return in;
}

bool ToolKey (Session& s, uint32_t code)
{
    const auto draw = [&s] (Tool tool) {
        if (ShapeTool (tool) || tool != s.tool)
            s.drawing.clear ();
        s.tool = tool;
        s.landscapeMode = LandscapeMode::DrawSource;
    };
    switch (code) {
        case 'V':
            s.tool = Tool::Select, s.drawing.clear ();
            s.landscapeMode = LandscapeMode::Select;
            return true;
        case 'L':
            s.tool = Tool::Polyline;
            s.landscapeMode = LandscapeMode::DrawSource;
            return true;
        case 'P':
            s.tool = Tool::Path;
            s.landscapeMode = LandscapeMode::DrawSource;
            return true;
        case 'B':
            s.tool = Tool::Curve; // the pen: clicks for corners, drags for curves
            s.landscapeMode = LandscapeMode::DrawSource;
            return true;
        case 'Q':
            draw (Tool::Rectangle);
            return true;
        case 'N':
            draw (Tool::Polygon);
            return true;
        case 'O':
            draw (Tool::Ellipse);
            return true;
        case 'M': // the selection stays: it is what the line mirrors
            s.tool = Tool::Mirror, s.drawing.clear ();
            s.landscapeMode = LandscapeMode::Select;
            if (s.selection.Items ().empty ())
                s.message = "Select what to mirror, then click the mirror line's two points.";
            return true;
        case 0x26: // Up and Down: the polygon's sides, while it is the tool
        case 0x28:
            if (s.tool != Tool::Polygon)
                return false;
            s.polygonSides = std::clamp (s.polygonSides + (code == 0x26 ? 1 : -1), 3, edit::kMaxPolygonSides);
            s.message = std::to_string (s.polygonSides) + " sides";
            return true;
        default:
            return false;
    }
}

namespace {
// The mirror line from `a` towards `b`, run across the view, and the selected objects'
// outlines as they would land: straight between their points, enough to aim by.
void MirrorPreview (Session& s, const Map& m, Vec3 a, Vec3 b, float scale)
{
    Vec3 dir;
    if (!edit::Normalize (Vec3 { b.x - a.x, b.y - a.y, 0.0 }, dir))
        return;
    const double reach = 2.0e3 * m.MetresPerPixel (m.v.x, m.v.y, a.z, a);
    DashedLine (s, m, a - dir * reach, a + dir * reach, kGuide, 1.5f * scale);
    Line (s, m, a, b, kGuide, 2.0f * scale);
    const auto reflect = [&] (Vec3 p) {
        const double along = 2.0 * ((p.x - a.x) * dir.x + (p.y - a.y) * dir.y);
        return Vec3 { a.x + along * dir.x - (p.x - a.x), a.y + along * dir.y - (p.y - a.y), p.z };
    };
    const auto run = [&] (const std::vector<Vec3>& points, bool closed) {
        for (size_t i = 0; i + 1 < points.size () + (closed ? 1 : 0); ++i)
            Line (s, m, reflect (points[i]), reflect (points[(i + 1) % points.size ()]), kAccent, 1.5f * scale);
    };
    const edit::Clipboard objects = edit::Copy (Doc (s), s.selection.Items ());
    for (const auto& e : objects.entities) {
        std::vector<Vec3> points;
        for (const auto& v : e.vertices)
            points.push_back (v.p);
        run (points, e.closed);
    }
    for (const auto& p : objects.paths) {
        std::vector<Vec3> points;
        for (const auto& c : p.points)
            points.push_back (c.p);
        run (points, false);
    }
}
} // namespace

// What a drawing tool has placed so far and where the pointer would put the next: the pen's
// handles, a shape's outline (its quarter Beziers sampled for an ellipse), a line's run.
void DrawingPreview (Session& s, const Map& m, bool shift, float scale)
{
    if (s.tool == Tool::Mirror) {
        if (!s.drawing.empty ())
            MirrorPreview (s, m, s.drawing.front (), s.snap.point, scale);
        for (const auto& q : s.drawing)
            Dot (s, m, q, 4.0f * scale, kGuide);
    }
    else if (s.tool == Tool::Curve)
        PenPreview (s, m, s.snap.point, scale);
    else if (ShapeTool (s.tool)) {
        edit::PrimitiveOutline shape;
        std::string why;
        if (!s.drawing.empty () && edit::OutlinePrimitive (ShapeInput (s, s.snap.point, shift), shape, why)) {
            const auto& q = shape.points;
            if (shape.out.empty ())
                for (size_t i = 0; i < q.size (); ++i)
                    Line (s, m, q[i], q[(i + 1) % q.size ()], kAccent, 2.0f * scale);
            else // the ellipse, its quarter Beziers sampled
                for (size_t i = 0; i < q.size (); ++i) {
                    const size_t j = (i + 1) % q.size ();
                    const Vec3 b = q[i] + shape.out[i], c = q[j] + shape.in[j];
                    Vec3 last = q[i];
                    for (int k = 1; k <= 12; ++k) {
                        const double t = k / 12.0, r = 1.0 - t;
                        const Vec3 at =
                            q[i] * (r * r * r) + b * (3 * r * r * t) + c * (3 * r * t * t) + q[j] * (t * t * t);
                        Line (s, m, last, at, kAccent, 2.0f * scale);
                        last = at;
                    }
                }
        }
        for (const auto& q : s.drawing)
            Dot (s, m, q, 4.0f * scale, kAccent);
    }
    else {
        for (size_t i = 0; i + 1 < s.drawing.size (); ++i)
            Line (s, m, s.drawing[i], s.drawing[i + 1], kAccent, 2.5f * scale);
        if (!s.drawing.empty ())
            Line (s, m, s.drawing.back (), s.snap.point, kAccent, 1.5f * scale);
        for (const auto& q : s.drawing)
            Dot (s, m, q, 4.0f * scale, kAccent);
    }
}

void FinishShape (Session& s, edit::Vec3 second, bool uniform)
{
    const auto input = ShapeInput (s, second, uniform);
    const std::string role = s.role;
    const auto landscapeRole = s.landscapeRole;
    edit::Id created = edit::kNoId;
    const bool ok = s.edits.Apply ([&] (edit::Document& d, std::string& error) {
        created = edit::AddPrimitive (d, input, role, error);
        return created != edit::kNoId && RegisterSource (d, created, false, landscapeRole, error);
    });
    if (!ok) {
        s.message = s.edits.Message (); // the click stays: a shape with no size is placed again
        return;
    }
    s.drawing.clear ();
    s.drawHandles.clear ();
    s.selection.Set ({ { created, edit::Part::Entity, 0 } });
    s.message.clear ();
}

void FinishMirror (Session& s, edit::Vec3 second, bool copy)
{
    if (s.drawing.empty () || edit::DistanceXY (s.drawing.front (), second) < 1.0e-6) {
        s.message = "Click a second point away from the first: the mirror line runs between them.";
        return;
    }
    // The line is no longer being drawn once it is complete: the mirror is what happens next.
    const edit::Vec3 first = s.drawing.front ();
    s.drawing.clear ();
    const uint64_t before = Doc (s).revision;
    MirrorSelection (s, first, second - first, copy);
    if (Doc (s).revision == before) {
        s.drawing.push_back (first); // refused: the message says why; the first click stays
        return;
    }
    s.tool = Tool::Select;
}

void FinishDrawing (Session& s)
{
    if (ShapeTool (s.tool) || s.tool == Tool::Mirror)
        return; // a shape or a mirror line ends at its second click, never at Enter or a double click
    if (s.drawing.size () < 2) {
        s.drawing.clear ();
        return;
    }
    auto points = s.drawing;
    auto handles = s.drawHandles;
    handles.resize (points.size ());
    const bool closed = points.size () > 3 && edit::Distance (points.front (), points.back ()) < 1.0e-6;
    if (closed)
        points.pop_back (), handles.pop_back ();
    edit::Id created = edit::kNoId;
    std::vector<edit::Id> createdPoints;
    const Tool tool = s.tool;
    const double width = s.pathWidth;
    const std::string role = s.role;
    const auto landscapeRole = s.landscapeRole;
    const bool ok = s.edits.Apply ([&] (edit::Document& d, std::string& error) {
        created =
            tool == Tool::Path ? edit::AddPath (d, points, width, role) : edit::AddPolyline (d, points, closed, role);
        if (tool == Tool::Path)
            for (const auto& point : edit::FindPath (d, created)->points)
                createdPoints.push_back (point.id);
        if (tool == Tool::Curve) {
            ApplyPenHandles (d, created, handles); // the pen's smooth anchors keep their handles
            // Closed, the curve runs on through its first anchor: tangent, not a corner.
            const edit::Entity* e = edit::Find (d, created);
            if (closed && e != nullptr && !handles.empty () && std::hypot (handles[0].x, handles[0].y) < 1.0e-12) {
                std::string why;
                edit::SetSmooth (d, created, e->vertices.front ().id, true, why);
            }
        }
        return RegisterSource (d, created, tool == Tool::Path, landscapeRole, error);
    });
    if (!ok) {
        s.message = s.edits.Message ();
        return;
    }
    s.drawing.clear ();
    s.drawHandles.clear ();
    // End may have joined the new run into an existing path. Select its surviving
    // identity, never the now-retired id returned by AddPath during the operation.
    if (tool == Tool::Path && edit::FindPath (Doc (s), created) == nullptr)
        for (const auto& path : Doc (s).paths)
            for (const auto& point : path.points)
                if (std::find (createdPoints.begin (), createdPoints.end (), point.id) != createdPoints.end ())
                    created = path.id;
    s.selection.Set ({ { created, edit::Part::Entity, 0 } });
    if (tool == Tool::Path && Doc (s).landscape) {
        s.terrainPath = created;
        s.profilePath = created;
    }
    s.message.clear ();
}

} // namespace geomsrv::archviz::edithud::detail
