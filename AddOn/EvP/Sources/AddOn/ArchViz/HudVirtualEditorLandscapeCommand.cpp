#include "ArchViz/HudVirtualEditor.hpp"
#include <algorithm>

namespace geomsrv::archviz::edithud {
namespace lg = tapioca::geometry;

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

const char* LandscapeCommandPrompt (const Session& s)
{
    if (!s.landscapeCommand)
        return "Choose a command, then select faces in the canvas.";
    return s.landscapeCommand->second
               ? "Select second set. Enter to finish; Esc to cancel. Click a face to add or remove it."
               : "Select first set. Enter to continue; Esc to cancel. Click a face to add or remove it.";
}

bool AdvanceLandscapeCommand (Session& s)
{
    if (!s.landscapeCommand)
        return false;
    Landscape (s); // A changed revision cancels rather than reusing stale face ranks.
    if (!s.landscapeCommand)
        return false;
    std::vector<lg::RegionDescriptor> selected;
    for (const auto& ref : s.selection.Items ())
        if (ref.part == edit::Part::LandscapeFace && ref.revision == s.landscapeRevision)
            for (const auto& face : s.landscape.faces)
                if (face.faceId == ref.item)
                    selected.push_back (face.descriptor);
    if (selected.empty ()) {
        s.message = "Select at least one face, then press Enter.";
        return false;
    }
    if (!s.landscapeCommand->second) {
        s.landscapeCommand->first = std::move (selected);
        s.landscapeCommand->second = true;
        s.selection.Clear ();
        s.message.clear ();
        return true;
    }
    const auto command = *s.landscapeCommand;
    edit::Id objectId = 0;
    // Both semantic operands, the Boolean and its output publish in one existing undo step.
    const bool ok = s.edits.Apply ([&] (edit::Document& doc, std::string& error) {
        lg::LandscapeOperation a, b, op;
        a.kind = b.kind = lg::OperationKind::RegionSelect;
        a.faces = command.first;
        b.faces = selected;
        edit::Id aId = 0, bId = 0, id = 0;
        if (!lg::AddLandscapeOperation (doc, a, aId, error) || !lg::AddLandscapeOperation (doc, b, bId, error))
            return false;
        op.kind = command.kind;
        op.inputs = { aId, bId };
        if (!lg::AddLandscapeOperation (doc, op, id, error))
            return false;
        lg::HardscapeObject object;
        object.name = command.kind == lg::OperationKind::Union        ? "Union"
                      : command.kind == lg::OperationKind::Difference ? "Subtract"
                                                                      : "Intersection";
        object.footprintOperation = id;
        return lg::AddHardscapeObject (doc, object, objectId, error);
    });
    if (!ok) {
        s.message = s.edits.Message ();
        return false;
    }
    CancelLandscapeCommand (s);
    s.landscapeObject = objectId;
    s.message = "Command complete. Sources remain editable.";
    Landscape (s);
    return true;
}
} // namespace geomsrv::archviz::edithud
