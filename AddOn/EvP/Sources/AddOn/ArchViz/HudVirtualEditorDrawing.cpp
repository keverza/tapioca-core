#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditShape.hpp"
#include <cmath>
#include <algorithm>

namespace geomsrv::archviz::edithud::detail {

void FinishDrawing (Session& s)
{
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
    const bool ok = s.edits.Apply ([&] (edit::Document& d, std::string&) {
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
        return true;
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
    s.message.clear ();
}

} // namespace geomsrv::archviz::edithud::detail
