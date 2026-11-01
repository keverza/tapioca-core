#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditLandscapeDomain.hpp"
#include <algorithm>

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

const char* LandscapeCommandPrompt (const Session& s)
{
    if (!s.landscapeCommand)
        return "Choose a command, then select faces in the canvas.";
    if (s.landscapeCommand->kind == lg::OperationKind::Union)
        return "Select two or more faces to merge. Enter to finish; Esc to cancel. Click a face to add or remove it.";
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
    // A union is one selection: what is merged has no subject and no operand, so asking for two
    // sets made the user pick the same kind of thing twice (the user, 2026-10-24). Its first
    // face is one input and the rest the other, so the result and its undo step are unchanged.
    const bool merge = s.landscapeCommand->kind == lg::OperationKind::Union;
    if (merge && selected.size () < 2) {
        s.message = "Select at least two faces to merge, then press Enter.";
        return false;
    }
    if (selected.empty ()) {
        s.message = "Select at least one face, then press Enter.";
        return false;
    }
    if (merge) {
        s.landscapeCommand->first = { selected.front () };
        selected.erase (selected.begin ());
    }
    else if (!s.landscapeCommand->second) {
        s.landscapeCommand->first = std::move (selected);
        s.landscapeCommand->second = true;
        s.selection.Clear ();
        s.message.clear ();
        return true;
    }
    const auto command = *s.landscapeCommand;
    // #112 P1: one domain command authors both semantic operands, the Boolean and
    // its output owner in the existing undo step; the subject's configured owner
    // (when its faces cite exactly one region) supplies the output metadata.
    lg::DomainCommand domain;
    domain.kind = command.kind;
    domain.name = command.kind == lg::OperationKind::Union        ? "Union"
                  : command.kind == lg::OperationKind::Difference ? "Subtract"
                                                                  : "Intersection";
    domain.subject.faces = command.first;
    domain.operand.faces = selected;
    edit::Id objectId = 0;
    const bool ok = s.edits.Apply ([&] (edit::Document& doc, std::string& error) {
        const auto applied = lg::ApplyDomainCommand (doc, domain);
        if (applied.ok)
            objectId = applied.objectId;
        else
            error = applied.error;
        return applied.ok;
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
