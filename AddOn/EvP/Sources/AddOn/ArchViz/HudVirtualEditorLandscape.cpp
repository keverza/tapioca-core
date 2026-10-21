#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "ArchViz/HudShell.hpp"
#include <algorithm>

namespace geomsrv::archviz::edithud {
namespace lg = tapioca::geometry;

namespace {
bool Ready (Session& s)
{
    if (!s.edits.Doc ().landscape || GizmoModeLocked (s) || !s.drawing.empty () || s.edits.InGesture ()) {
        s.message = "Finish the current input or Esc it before a landscape action.";
        return false;
    }
    return true;
}
edit::Ref FaceRef (const lg::DerivedRegion& face, uint64_t revision)
{
    return { edit::kNoId, edit::Part::LandscapeFace, face.faceId, revision };
}
} // namespace

const lg::LandscapeResult& Landscape (Session& s)
{
    const auto& doc = s.edits.Doc ();
    if (s.landscapeRevision == doc.revision)
        return s.landscape;
    // No face rank survives a document edit, undo or reload. Retained authored choices
    // live in RegionSelect descriptors, never in this transient Selection.
    auto refs = s.selection.Items ();
    refs.erase (std::remove_if (refs.begin (), refs.end (),
                                [] (const auto& ref) { return ref.part == edit::Part::LandscapeFace; }),
                refs.end ());
    s.selection.Set (refs);
    if (s.selection.hover.part == edit::Part::LandscapeFace)
        s.selection.hover = {};
    s.landscape = {};
    s.landscapeRevision = doc.revision;
    ++s.landscapeBuilds;
    if (doc.landscape && !lg::EvaluateLandscape (doc, s.landscape)) {
        s.message = s.landscape.error;
        s.landscape.faces.clear ();
        s.landscape.regions.clear ();
    }
    if (doc.landscape && std::none_of (doc.landscape->objects.begin (), doc.landscape->objects.end (),
                                       [&] (const auto& o) { return o.id == s.landscapeObject; }))
        s.landscapeObject = edit::kNoId;
    return s.landscape;
}

bool SetLandscapeMode (Session& s, LandscapeMode mode)
{
    if (!Ready (s))
        return false;
    s.landscapeMode = mode;
    s.tool = mode == LandscapeMode::DrawSource ? Tool::Polyline : Tool::Select;
    s.selection.Clear ();
    s.selection.hover = {};
    s.hoverHandle = -1;
    s.message.clear ();
    return true;
}

bool PickLandscapeFace (Session& s, edit::Vec3 point, bool click, bool additive)
{
    const auto& result = Landscape (s);
    if (!s.edits.Doc ().landscape ||
        (s.landscapeMode != LandscapeMode::Face && s.landscapeMode != LandscapeMode::Boolean))
        return false;
    const auto* face = lg::LandscapeFaceAt (result, point);
    s.selection.hover = face ? FaceRef (*face, s.landscapeRevision) : edit::Ref {};
    if (click) {
        if (!face) {
            if (!additive)
                s.selection.Clear ();
        }
        else if (additive)
            s.selection.Toggle (s.selection.hover);
        else
            s.selection.Set ({ s.selection.hover });
    }
    return face != nullptr;
}

bool KeepLandscapeFaces (Session& s, bool subtract)
{
    if (!Ready (s))
        return false;
    const auto& result = Landscape (s);
    lg::LandscapeOperation operation;
    operation.kind = lg::OperationKind::RegionSelect;
    for (const auto& ref : s.selection.Items ())
        if (ref.part == edit::Part::LandscapeFace && ref.revision == s.landscapeRevision)
            for (const auto& face : result.faces)
                if (face.faceId == ref.item)
                    operation.faces.push_back (face.descriptor);
    if (operation.faces.empty ()) {
        s.message = "Select a current face to keep.";
        return false;
    }
    edit::Id objectId = s.landscapeObject;
    if (subtract && !objectId) {
        s.message = "Choose a kept object before removing a face.";
        return false;
    }
    const bool ok = s.edits.Apply ([&] (edit::Document& doc, std::string& error) {
        edit::Id op = 0;
        if (!lg::AddLandscapeOperation (doc, operation, op, error))
            return false;
        if (objectId) {
            auto object = std::find_if (doc.landscape->objects.begin (), doc.landscape->objects.end (),
                                        [=] (const auto& o) { return o.id == objectId; });
            lg::LandscapeOperation unite;
            unite.kind = subtract ? lg::OperationKind::Difference : lg::OperationKind::Union;
            unite.inputs = { object->footprintOperation, op };
            if (!lg::AddLandscapeOperation (doc, unite, op, error))
                return false;
            object = std::find_if (doc.landscape->objects.begin (), doc.landscape->objects.end (),
                                   [=] (const auto& o) { return o.id == objectId; });
            object->footprintOperation = op;
            return true;
        }
        lg::HardscapeObject object;
        object.name = "Hardscape";
        object.footprintOperation = op;
        return lg::AddHardscapeObject (doc, object, objectId, error);
    });
    s.message = ok ? "Sources remain editable." : s.edits.Message ();
    if (ok)
        s.landscapeObject = objectId;
    Landscape (s);
    return ok;
}

bool BooleanLandscape (Session& s, lg::OperationKind kind, edit::Id cutter)
{
    if (!Ready (s))
        return false;
    Landscape (s);
    if (kind != lg::OperationKind::Union && kind != lg::OperationKind::Difference &&
        kind != lg::OperationKind::Intersection)
        return false;
    if (!s.landscapeObject) {
        s.message = "Keep a face and choose its object first.";
        return false;
    }
    const edit::Id objectId = s.landscapeObject;
    const bool ok = s.edits.Apply ([=] (edit::Document& doc, std::string& error) {
        const auto object = std::find_if (doc.landscape->objects.begin (), doc.landscape->objects.end (),
                                          [=] (const auto& o) { return o.id == objectId; });
        lg::LandscapeOperation op;
        op.kind = kind;
        op.inputs = { object->footprintOperation, cutter };
        edit::Id id = 0;
        if (!lg::AddLandscapeOperation (doc, op, id, error))
            return false;
        // AddLandscapeOperation publishes a new document value, invalidating iterators.
        for (auto& o : doc.landscape->objects)
            if (o.id == objectId)
                o.footprintOperation = id;
        return true;
    });
    s.message = ok ? "Sources remain editable." : s.edits.Message ();
    Landscape (s);
    return ok;
}

bool ReloadLandscape (Session& s, const std::string& definition)
{
    if (!Ready (s))
        return false;
    edit::Document loaded;
    if (!lg::ReadLandscape (definition, loaded, s.message))
        return false;
    // A reload is a virtual document edit, not a second session/history or allocator reset.
    const bool ok = s.edits.Apply ([loaded] (edit::Document& doc, std::string&) {
        const auto highWater = doc.nextId;
        doc = loaded;
        doc.nextId = std::max (doc.nextId, highWater);
        return true;
    });
    s.message = ok ? "Virtual definition reloaded. No model writes." : s.edits.Message ();
    if (ok)
        s.selection.Clear ();
    Landscape (s);
    return ok;
}

bool detail::LandscapeView (Session& s, const Map& m, ImVec2 mouse, bool hovered, float scale)
{
    if (!s.edits.Doc ().landscape)
        return false;
    const auto& result = Landscape (s);
    if (s.landscapeMode != LandscapeMode::Face && s.landscapeMode != LandscapeMode::Boolean)
        return false;
    edit::Vec3 point;
    if (hovered && m.Onto (mouse.x, mouse.y, s.plane.origin.z, {}, point)) {
        const bool click = ImGui::IsMouseClicked (0);
        PickLandscapeFace (s, point, click, ImGui::GetIO ().KeyShift);
        if (click && m.v.ctrl)
            KeepLandscapeFaces (s, true);
    }
    else
        s.selection.hover = {};
    for (const auto& face : result.faces)
        if (s.selection.hover.part == edit::Part::LandscapeFace && s.selection.hover.item == face.faceId) {
            const auto rgba = editlayers::Style {}.landscapeHover;
            const auto colour = IM_COL32 (rgba >> 24, (rgba >> 16) & 255, (rgba >> 8) & 255, 255);
            for (const auto& ring : face.rings)
                for (size_t i = 0; i < ring.size (); ++i) {
                    auto a = ring[i], b = ring[(i + 1) % ring.size ()];
                    a.z = b.z = s.plane.origin.z;
                    Line (s, m, a, b, colour, 3.5f * scale);
                }
        }
    if (m.v.pointer && !m.v.overHud) {
        hudshell::OwnCursor (false);
        ImGui::SetMouseCursor (ImGuiMouseCursor_Arrow);
    }
    return true;
}
} // namespace geomsrv::archviz::edithud
