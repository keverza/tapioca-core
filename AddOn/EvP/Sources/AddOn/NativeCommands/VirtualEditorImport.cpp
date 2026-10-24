#include "APIEnvir.h"
#include "ACAPinc.h"
#include "NativeCommands/VirtualEditorImport.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "Edit/EditImport.hpp"
#include "Edit/EditLandscape.hpp"
#include "Edit/EditLandscapeEntrance.hpp"
#include <algorithm>
#include <cmath>
#include <variant>
#include <vector>

// OVERLAY-INVARIANTS.md §§8,12: a queued main-thread read for the open editor session only; no
// camera, renderer or input state, and nothing outlives the call.
namespace geomsrv {
namespace {
namespace eh = archviz::edithud;
using Curve = std::variant<edit::ImportedCurve, edit::ImportedEllipse>;
// A shape read from one element, and the element's canonical GUID.
struct Shape {
    Curve curve;
    std::string guid;
};

struct Counts {
    size_t added = 0, unsupported = 0, elliptic = 0, unreadable = 0, degenerate = 0, open = 0, away = 0;
};

std::string Canonical (const API_Guid& guid)
{
    std::string canonical;
    tapioca::geometry::CanonicalLandscapeGuid (APIGuidToString (guid).ToCStr ().Get (), canonical);
    return canonical;
}

bool Closed (const Curve& curve)
{
    return std::holds_alternative<edit::ImportedEllipse> (curve) ? std::get<edit::ImportedEllipse> (curve).whole
                                                                 : std::get<edit::ImportedCurve> (curve).closed;
}

// One polygon's contours from its memo (1-based coordinates; each contour repeats its first point
// at its end; arcs named by their start and end coordinate indices).
bool Contours (const API_Guid& guid, bool closedOnly, double z, std::vector<Curve>& out)
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

void ReadCurves (const API_Neig& neig, double z, std::vector<Curve>& out, Counts& counts)
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

void Read (const API_Neig& neig, double z, std::vector<Shape>& out, Counts& counts)
{
    std::vector<Curve> curves;
    ReadCurves (neig, z, curves, counts);
    const auto guid = Canonical (neig.guid);
    for (auto& curve : curves)
        out.push_back ({ std::move (curve), guid });
}

// The storey's elevation, metres; 0 when the settings cannot be read.
double StoreyLevel (short index)
{
    API_StoryInfo info {};
    double level = 0.0;
    if (ACAPI_ProjectSetting_GetStorySettings (&info) != NoError || info.data == nullptr)
        return level;
    for (short i = 0; i <= info.lastStory - info.firstStory; ++i)
        if ((*info.data)[i].index == index)
            level = (*info.data)[i].level;
    BMKillHandle (reinterpret_cast<GSHandle*> (&info.data));
    return level;
}

// A door's threshold: its plan centre on its straight wall's reference line, at the wall's
// storey and base plus its sill. False for a door in a curved or polygonal wall.
bool DoorThreshold (const API_Guid& guid, edit::Vec3& at)
{
    API_Element door {};
    door.header.guid = guid;
    if (ACAPI_Element_Get (&door) != NoError || door.header.type.typeID != API_DoorID)
        return false;
    API_Element wall {};
    wall.header.guid = door.door.owner;
    if (ACAPI_Element_Get (&wall) != NoError || wall.header.type.typeID != API_WallID ||
        std::abs (wall.wall.angle) > 1e-9)
        return false;
    const double dx = wall.wall.endC.x - wall.wall.begC.x, dy = wall.wall.endC.y - wall.wall.begC.y;
    const double length = std::hypot (dx, dy);
    if (!(length > 0))
        return false;
    at = { wall.wall.begC.x + dx / length * door.door.objLoc, wall.wall.begC.y + dy / length * door.door.objLoc,
           StoreyLevel (wall.header.floorInd) + wall.wall.bottomOffset + door.door.lower };
    return true;
}

bool ImportEntrances (eh::Session& s, const GS::Array<API_Neig>& neigs, std::string& message)
{
    struct Door {
        edit::Vec3 at;
        std::string guid;
    };
    std::vector<Door> doors;
    size_t other = 0;
    for (const API_Neig& neig : neigs) {
        Door door;
        if (DoorThreshold (neig.guid, door.at))
            door.guid = Canonical (neig.guid), doors.push_back (std::move (door));
        else
            ++other;
    }
    if (doors.empty ()) {
        message = "Select Archicad doors in straight walls to add them as entrances.";
        return false;
    }
    size_t added = 0, away = 0;
    const bool ok = s.edits.Apply ([&] (edit::Document& d, std::string& error) {
        added = away = 0;
        for (const auto& door : doors) {
            // A door stands in its wall: its building's outline passes within a wall's width.
            const auto building = tapioca::geometry::BuildingNear (d, door.at, 1.0);
            if (!building) {
                ++away;
                continue;
            }
            tapioca::geometry::Id made = 0;
            if (!tapioca::geometry::AddLandscapeEntrance (d, building, door.at, door.at.z, door.guid, made, error))
                return false;
            ++added;
        }
        if (!added)
            error = "No selected door stands on a building outline: set the building's outline first.";
        return added > 0;
    });
    if (!ok) {
        message = s.edits.Message ();
        return false;
    }
    message = std::to_string (added) + (added == 1 ? " entrance" : " entrances") + " added from Archicad doors";
    if (away)
        message += "; " + std::to_string (away) + " not on a building outline";
    if (other)
        message += "; " + std::to_string (other) + " skipped (not a door in a straight wall)";
    message += ".";
    return true;
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
    skip (c.open, "not closed");
    skip (c.away, "a property line is one closed shape");
    return text + ".";
}
} // namespace

bool ImportSelectedDrafting (eh::Session& s, DraftingImport as, std::string& message)
{
    namespace lg = tapioca::geometry;
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
        message = as == DraftingImport::Entrances ? "Select Archicad doors first."
                                                  : "Select Archicad lines, polylines, arcs, circles or fills first.";
        return false;
    }
    if (as != DraftingImport::Lines && !s.edits.Doc ().landscape) {
        message = "Property lines, buildings and entrances belong to a landscape: start one first.";
        return false;
    }
    if (as == DraftingImport::Entrances)
        return ImportEntrances (s, neigs, message);
    const double z = s.plane.origin.z;
    std::vector<Shape> shapes;
    Counts counts;
    for (const API_Neig& neig : neigs)
        Read (neig, z, shapes, counts);
    // A property line or a building is a closed outline; a property line is exactly one.
    if (as != DraftingImport::Lines) {
        const auto open =
            std::remove_if (shapes.begin (), shapes.end (), [] (const Shape& shape) { return !Closed (shape.curve); });
        counts.open += size_t (shapes.end () - open);
        shapes.erase (open, shapes.end ());
    }
    if (as == DraftingImport::Property && shapes.size () > 1) {
        counts.away = shapes.size () - 1;
        shapes.resize (1);
    }
    if (shapes.empty ()) {
        message = Say (counts);
        return false;
    }
    const lg::SourceRole role = as == DraftingImport::Property   ? lg::SourceRole::PropertyLine
                                : as == DraftingImport::Building ? lg::SourceRole::Building
                                                                 : lg::SourceRole::Guide;
    std::vector<edit::Id> made;
    const bool ok = s.edits.Apply ([&] (edit::Document& d, std::string& error) {
        made.clear ();
        counts.added = counts.degenerate = 0;
        for (const auto& shape : shapes) {
            std::string why;
            const edit::Id id =
                std::holds_alternative<edit::ImportedCurve> (shape.curve)
                    ? edit::AddImportedCurve (d, std::get<edit::ImportedCurve> (shape.curve), "imported", why)
                    : edit::AddImportedEllipse (d, std::get<edit::ImportedEllipse> (shape.curve), "imported", why);
            if (id == edit::kNoId) {
                ++counts.degenerate;
                continue;
            }
            if (d.landscape) {
                if (!lg::SetLandscapeSourceRole (d, id, role, error))
                    return false;
                // The source keeps the element it references, for a later refresh from Archicad.
                for (auto& source : d.landscape->sources)
                    if (source.entityId == id && !source.deleted)
                        source.archicadGuid = shape.guid;
            }
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
