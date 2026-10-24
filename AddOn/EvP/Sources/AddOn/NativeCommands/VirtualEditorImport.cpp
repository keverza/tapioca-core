#include "APIEnvir.h"
#include "ACAPinc.h"
#include "NativeCommands/VirtualEditorImport.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "Edit/EditImport.hpp"
#include "Edit/EditLandscape.hpp"
#include <cmath>
#include <variant>
#include <vector>

// OVERLAY-INVARIANTS.md §§8,12: a queued main-thread read for the open editor session only; no
// camera, renderer or input state, and nothing outlives the call.
namespace geomsrv {
namespace {
namespace eh = archviz::edithud;
using Shape = std::variant<edit::ImportedCurve, edit::ImportedEllipse>;

struct Counts {
    size_t added = 0, unsupported = 0, elliptic = 0, unreadable = 0, degenerate = 0;
};

// One polygon's contours from its memo (1-based coordinates; each contour repeats its first point
// at its end; arcs named by their start and end coordinate indices).
bool Contours (const API_Guid& guid, bool closedOnly, double z, std::vector<Shape>& out)
{
    API_ElementMemo memo {};
    if (ACAPI_Element_GetMemo (guid, &memo, APIMemoMask_Polygon) != NoError || memo.coords == nullptr ||
        memo.pends == nullptr) {
        ACAPI_DisposeElemMemoHdls (&memo);
        return false;
    }
    const Int32 coords = Int32 (BMGetHandleSize (reinterpret_cast<GSHandle> (memo.coords)) / sizeof (API_Coord));
    const Int32 ends = Int32 (BMGetHandleSize (reinterpret_cast<GSHandle> (memo.pends)) / sizeof (Int32));
    const Int32 arcs = memo.parcs == nullptr
                           ? 0
                           : Int32 (BMGetHandleSize (reinterpret_cast<GSHandle> (memo.parcs)) / sizeof (API_PolyArc));
    bool ok = ends >= 2;
    for (Int32 sub = 1; ok && sub < ends; ++sub) {
        const Int32 first = (*memo.pends)[sub - 1] + 1, last = (*memo.pends)[sub];
        if (first < 1 || last >= coords || last - first < 1) {
            ok = false;
            break;
        }
        edit::ImportedCurve curve;
        for (Int32 i = first; i <= last; ++i)
            curve.points.push_back ({ (*memo.coords)[i].x, (*memo.coords)[i].y, z });
        const auto& a = (*memo.coords)[first];
        const auto& b = (*memo.coords)[last];
        curve.closed = closedOnly || std::hypot (a.x - b.x, a.y - b.y) <= 1e-9;
        curve.arcs.assign (curve.points.size (), 0.0);
        for (Int32 k = 0; k < arcs; ++k) {
            const API_PolyArc& arc = (*memo.parcs)[k];
            if (arc.begIndex >= first && arc.begIndex < last && arc.endIndex == arc.begIndex + 1)
                curve.arcs[size_t (arc.begIndex - first)] = arc.arcAngle;
        }
        out.push_back (std::move (curve));
    }
    ACAPI_DisposeElemMemoHdls (&memo);
    return ok;
}

void Read (const API_Neig& neig, double z, std::vector<Shape>& out, Counts& counts)
{
    API_Element element {};
    element.header.guid = neig.guid;
    if (ACAPI_Element_Get (&element) != NoError) {
        ++counts.unreadable;
        return;
    }
    switch (element.header.type.typeID) {
        case API_LineID: {
            edit::ImportedCurve line;
            line.points = { { element.line.begC.x, element.line.begC.y, z },
                            { element.line.endC.x, element.line.endC.y, z } };
            out.push_back (std::move (line));
            return;
        }
        case API_PolyLineID:
        case API_HatchID:
            // A fill's contours are its outline and its holes, each closed.
            if (!Contours (element.header.guid, element.header.type.typeID == API_HatchID, z, out))
                ++counts.unreadable;
            return;
        case API_ArcID:
        case API_CircleID: {
            const API_ArcType& arc = element.header.type.typeID == API_ArcID ? element.arc : element.circle;
            // ⚠️ ONLY CIRCULAR ONES. The DevKit names `ratio` 'a/b' and `angle` the 'a' axis's direction
            // but not which axis `r` measures; an ellipse is skipped and counted, not guessed at.
            if (std::abs (arc.ratio - 1.0) > 1e-9) {
                ++counts.elliptic;
                return;
            }
            edit::ImportedEllipse circle;
            circle.centre = { arc.origC.x, arc.origC.y, z };
            circle.radius = arc.r;
            circle.whole = element.header.type.typeID == API_CircleID || arc.whole;
            circle.from = arc.begAng;
            circle.to = arc.endAng;
            out.push_back (circle);
            return;
        }
        default:
            ++counts.unsupported;
            return;
    }
}

std::string Say (const Counts& c)
{
    std::string text = std::to_string (c.added) + (c.added == 1 ? " line" : " lines") + " added from Archicad";
    const auto skip = [&text] (size_t n, const char* why) {
        if (n > 0)
            text += "; " + std::to_string (n) + " skipped (" + why + ")";
    };
    skip (c.unsupported, "not a line, polyline, arc, circle or fill");
    skip (c.elliptic, "elliptic arcs are not imported yet");
    skip (c.unreadable, "could not be read");
    skip (c.degenerate, "no length");
    return text + ".";
}
} // namespace

bool ImportSelectedDrafting (eh::Session& s, std::string& message)
{
    if (s.edits.InGesture () || !s.drawing.empty () || eh::GizmoModeLocked (s)) {
        message = "Finish the current input or Esc it before adding Archicad lines.";
        return false;
    }
    API_SelectionInfo info {};
    GS::Array<API_Neig> neigs;
    const GSErrCode selected = ACAPI_Selection_Get (&info, &neigs, false);
    if (info.marquee.coords != nullptr)
        BMKillHandle (reinterpret_cast<GSHandle*> (&info.marquee.coords));
    if ((selected != NoError && selected != APIERR_NOSEL) || neigs.IsEmpty ()) {
        message = "Select Archicad lines, polylines, arcs, circles or fills first.";
        return false;
    }
    const double z = s.plane.origin.z;
    std::vector<Shape> shapes;
    Counts counts;
    for (const API_Neig& neig : neigs)
        Read (neig, z, shapes, counts);
    if (shapes.empty ()) {
        message = Say (counts);
        return false;
    }
    std::vector<edit::Id> made;
    const bool ok = s.edits.Apply ([&] (edit::Document& d, std::string& error) {
        made.clear ();
        counts.added = counts.degenerate = 0;
        for (const auto& shape : shapes) {
            std::string why;
            const edit::Id id =
                std::holds_alternative<edit::ImportedCurve> (shape)
                    ? edit::AddImportedCurve (d, std::get<edit::ImportedCurve> (shape), "imported", why)
                    : edit::AddImportedEllipse (d, std::get<edit::ImportedEllipse> (shape), "imported", why);
            if (id == edit::kNoId) {
                ++counts.degenerate;
                continue;
            }
            if (d.landscape &&
                !tapioca::geometry::SetLandscapeSourceRole (d, id, tapioca::geometry::SourceRole::Guide, error))
                return false;
            made.push_back (id);
            ++counts.added;
        }
        if (made.empty ())
            error = "Nothing selected had a length to add.";
        return !made.empty ();
    });
    if (!ok) {
        message = s.edits.Message ();
        return false;
    }
    std::vector<edit::Ref> refs;
    for (const auto id : made)
        refs.push_back ({ id, edit::Part::Entity, 0 });
    s.selection.Set (refs);
    message = Say (counts);
    return true;
}

} // namespace geomsrv
