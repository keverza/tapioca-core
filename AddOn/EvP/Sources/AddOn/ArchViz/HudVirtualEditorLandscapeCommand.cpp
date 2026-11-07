#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditLandscapeDomain.hpp"
#include <algorithm>
#include <cmath>

namespace geomsrv::archviz::edithud {
namespace lg = tapioca::geometry;

bool detail::LandscapeCommandKeys (Session& s, const overlayhud::Input& v)
{
    if (!s.landscapeCommand)
        return false;
    for (const auto action : v.editActions)
        if (action == overlayhud::Input::EditAction::Cancel)
            CancelLandscapeCommand (s);
    for (const auto& key : v.keys)
        if (key.down && !key.repeat) {
            if (key.code == 0x1B)
                CancelLandscapeCommand (s);
            else if (key.code == 0x0D)
                AdvanceLandscapeCommand (s);
        }
    return true;
}

bool BeginLandscapeCommand (Session& s, lg::OperationKind kind)
{
    if (kind != lg::OperationKind::Union && kind != lg::OperationKind::Difference &&
        kind != lg::OperationKind::Intersection)
        return false;
    if (!SetLandscapeMode (s, LandscapeMode::Boolean))
        return false;
    s.landscapeCommand = LandscapeCommand { kind, s.edits.Doc ().revision };
    return true;
}

bool CancelLandscapeCommand (Session& s)
{
    if (!s.landscapeCommand)
        return false;
    s.landscapeCommand.reset ();
    s.landscapeMode = LandscapeMode::Select;
    s.selection.Clear ();
    s.selection.hover = {};
    s.message = "Command cancelled.";
    return true;
}

namespace {
const char* CommandName (lg::OperationKind kind)
{
    return kind == lg::OperationKind::Union        ? "Union"
           : kind == lg::OperationKind::Difference ? "Subtract"
                                                   : "Intersection";
}
bool Inside (edit::Vec3 point, const std::vector<std::vector<edit::Vec3>>& rings)
{
    bool inside = false;
    for (const auto& ring : rings)
        for (size_t i = 0, j = ring.size () - 1; i < ring.size (); j = i++) {
            const auto& a = ring[i];
            const auto& b = ring[j];
            if ((a.y > point.y) != (b.y > point.y) && point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x)
                inside = !inside;
        }
    return inside;
}
double RingArea (const std::vector<edit::Vec3>& ring)
{
    double area = 0;
    for (size_t i = 0, j = ring.size () - 1; i < ring.size (); j = i++)
        area += (ring[j].x - ring[i].x) * (ring[j].y + ring[i].y);
    return std::abs (area) / 2;
}
// Roles whose closed line is a planar area the DAG can combine. A reference line evaluates to no
// planar value, the property line is the site limit, and terrain/path sources have no curve.
bool PickableRole (lg::SourceRole role)
{
    switch (role) {
        case lg::SourceRole::Guide:
        case lg::SourceRole::Cutter:
        case lg::SourceRole::MaterialBoundary:
        case lg::SourceRole::GradingLimit:
        case lg::SourceRole::ProtectedZone:
        case lg::SourceRole::Building:
            return true;
        default:
            return false;
    }
}
// A closed reference line under the point: picking it is refused with a reason, not ignored.
bool ReferenceShapeAt (Session& s, edit::Vec3 point)
{
    const auto& doc = s.edits.Doc ();
    for (const auto& source : doc.landscape->sources) {
        if (source.deleted || source.role != lg::SourceRole::ReferenceLine || source.pathId)
            continue;
        std::vector<edit::Vec3> points;
        bool closed = false;
        std::string error;
        if (lg::LandscapeSourcePreview (doc, source, points, closed, error) && closed && points.size () > 2 &&
            Inside (point, { points }))
            return true;
    }
    return false;
}
const LandscapeShape* ShapeAt (Session& s, edit::Vec3 point)
{
    const LandscapeShape* best = nullptr;
    for (const auto& shape : LandscapeCommandShapes (s))
        if (Inside (point, shape.rings) && (!best || shape.area < best->area))
            best = &shape;
    return best;
}
} // namespace

const std::vector<LandscapeShape>& LandscapeCommandShapes (Session& s)
{
    static const std::vector<LandscapeShape> none;
    const auto& result = Landscape (s); // Cancels a command whose revision moved on.
    if (!s.landscapeCommand || !s.edits.Doc ().landscape)
        return none;
    auto& command = *s.landscapeCommand;
    if (command.shapesReady)
        return command.shapes;
    command.shapesReady = true;
    const auto& doc = s.edits.Doc ();
    for (const auto& object : doc.landscape->objects) {
        if (!object.footprintOperation)
            continue;
        LandscapeShape shape;
        shape.pick.object = object.id;
        for (const auto& region : result.regions)
            if (region.objectId == object.id) {
                shape.rings.insert (shape.rings.end (), region.rings.begin (), region.rings.end ());
                shape.area += region.area;
            }
        if (!shape.rings.empty ())
            command.shapes.push_back (std::move (shape));
    }
    for (const auto& source : doc.landscape->sources) {
        if (source.deleted || source.pathId || !PickableRole (source.role) ||
            std::any_of (doc.landscape->objects.begin (), doc.landscape->objects.end (),
                         [&] (const auto& o) { return o.boundarySource == source.id; }))
            continue;
        std::vector<edit::Vec3> points;
        bool closed = false;
        std::string error;
        if (!lg::LandscapeSourcePreview (doc, source, points, closed, error) || !closed || points.size () < 3)
            continue;
        LandscapeShape shape;
        shape.pick.source = source.id;
        shape.area = RingArea (points);
        shape.rings = { std::move (points) };
        command.shapes.push_back (std::move (shape));
    }
    return command.shapes;
}

bool PickLandscapeShape (Session& s, edit::Vec3 point, bool click)
{
    if (!s.landscapeCommand)
        return false;
    const auto* shape = ShapeAt (s, point);
    if (!s.landscapeCommand)
        return false;
    auto& command = *s.landscapeCommand;
    command.hover = shape ? std::optional<LandscapePick> (shape->pick) : std::nullopt;
    if (!click)
        return shape != nullptr;
    if (!shape) {
        s.message = ReferenceShapeAt (s, point)
                        ? "That shape is a reference line. Give it the Region boundary role first, then pick it."
                        : "No shape here. Pick inside a region or a closed line.";
        return false;
    }
    const auto found = std::find (command.picks.begin (), command.picks.end (), shape->pick);
    if (found != command.picks.end ())
        command.picks.erase (found);
    else
        command.picks.push_back (shape->pick);
    s.message.clear ();
    return true;
}

const char* LandscapeCommandPrompt (const Session& s)
{
    if (!s.landscapeCommand)
        return "Choose a command, then pick whole shapes in the canvas.";
    const auto& command = *s.landscapeCommand;
    if (command.kind == lg::OperationKind::Union)
        return "Pick two or more shapes to merge. Enter to finish; Esc to cancel. Click a picked shape to drop it.";
    if (command.picks.empty ())
        return command.kind == lg::OperationKind::Difference ? "Pick the shape to keep first. Esc to cancel."
                                                             : "Pick the first shape. Esc to cancel.";
    return command.kind == lg::OperationKind::Difference
               ? "Now pick the shape(s) to subtract. Enter to finish; Esc to cancel."
               : "Now pick the shape(s) to intersect with. Enter to finish; Esc to cancel.";
}

bool AdvanceLandscapeCommand (Session& s)
{
    if (!s.landscapeCommand)
        return false;
    Landscape (s); // A changed revision cancels rather than reusing stale picks.
    if (!s.landscapeCommand)
        return false;
    const auto command = *s.landscapeCommand;
    if (command.picks.size () < 2) {
        s.message = command.kind == lg::OperationKind::Union ? "Pick at least two shapes to merge, then press Enter."
                    : command.picks.empty ()                 ? "Pick the first shape, then the shape(s) to combine."
                                                             : "Pick one more shape, then press Enter.";
        return false;
    }
    std::vector<lg::Id> inputs;
    for (const auto& pick : command.picks) {
        lg::Id input = pick.source;
        if (pick.object)
            for (const auto& object : s.edits.Doc ().landscape->objects)
                if (object.id == pick.object)
                    input = object.footprintOperation;
        if (!input) {
            s.message = "A picked region has no footprint. Pick again.";
            return false;
        }
        inputs.push_back (input);
    }
    // #112 P1 / #160: the same domain command as before, fed whole operands. The first call authors
    // the output object (subject metadata, never an operand's); each further operand folds into
    // that object's root, so A op B op C stays one undo step and every source stays editable.
    edit::Id objectId = 0;
    const bool ok = s.edits.Apply ([&] (edit::Document& doc, std::string& error) {
        objectId = 0;
        lg::Id root = inputs.front ();
        for (size_t i = 1; i < inputs.size (); ++i) {
            lg::DomainCommand domain;
            domain.kind = command.kind;
            domain.name = CommandName (command.kind);
            domain.owner = objectId;
            domain.subject.input = root;
            domain.operand.input = inputs[i];
            const auto applied = lg::ApplyDomainCommand (doc, domain);
            if (!applied.ok) {
                error = applied.error;
                return false;
            }
            objectId = applied.objectId;
            root = applied.operationId;
        }
        return true;
    });
    if (!ok) {
        s.message = s.edits.Message ();
        return false;
    }
    CancelLandscapeCommand (s);
    s.landscapeObject = objectId;
    s.message = std::string (CommandName (command.kind)) + " complete. Sources remain editable.";
    Landscape (s);
    return true;
}
} // namespace geomsrv::archviz::edithud
