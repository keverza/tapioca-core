#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditShape.hpp"
#include <cmath>

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
    s.drawing.clear ();
    s.drawHandles.clear ();
    const bool closed = points.size () > 3 && edit::Distance (points.front (), points.back ()) < 1.0e-6;
    if (closed)
        points.pop_back (), handles.pop_back ();
    edit::Id created = edit::kNoId;
    const Tool tool = s.tool;
    const double width = s.pathWidth;
    const std::string role = s.role;
    const bool ok = s.edits.Apply ([&] (edit::Document& d, std::string&) {
        created =
            tool == Tool::Path ? edit::AddPath (d, points, width, role) : edit::AddPolyline (d, points, closed, role);
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
    s.selection.Set ({ { created, edit::Part::Entity, 0 } });
    s.message.clear ();
}

} // namespace geomsrv::archviz::edithud::detail
