// The boolean draft (HudVirtualEditor.hpp BooleanDraft): union, subtract, intersect or exclude of
// the selected closed outlines, previewed on a copy of the document and applied as one edit.
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditShape.hpp"
#include <algorithm>

namespace geomsrv::archviz::edithud::detail {
namespace {

constexpr double kPreviewMaxAngle = 0.1;

// The result as it would be applied: the same edit on a copy. Its rings, or why it refuses.
void Preview (const edit::Document& doc, BooleanDraft& draft)
{
    draft.rings.clear ();
    draft.error.clear ();
    edit::Document copy = doc;
    std::vector<edit::Id> made;
    if (!edit::BooleanEntities (copy, draft.operands, draft.kind, draft.error, &made))
        return;
    for (const edit::Id id : made)
        if (const edit::Entity* e = edit::Find (copy, id))
            for (auto& ring : edit::ShapeRings (*e, kPreviewMaxAngle))
                draft.rings.push_back (std::move (ring));
}

const char* Title (edit::BooleanKind kind)
{
    switch (kind) {
        case edit::BooleanKind::Union:
            return "Union";
        case edit::BooleanKind::Subtract:
            return "Subtract";
        case edit::BooleanKind::Intersect:
            return "Intersect";
        case edit::BooleanKind::Exclude:
            return "Exclude";
    }
    return "Boolean";
}

} // namespace

std::vector<edit::Id> BooleanOperands (const edit::Document& doc, const std::vector<edit::Ref>& refs)
{
    std::vector<edit::Id> out;
    for (const auto& r : refs) {
        const edit::Entity* e = edit::Find (doc, r.entity);
        if (r.part != edit::Part::Entity || e == nullptr || !e->closed)
            return {};
        if (std::find (out.begin (), out.end (), r.entity) == out.end ())
            out.push_back (r.entity);
    }
    return out.size () >= 2 ? out : std::vector<edit::Id> {};
}

bool CheckBooleanDraft (Session& s)
{
    if (!s.booleanDraft)
        return false;
    const auto& draft = *s.booleanDraft;
    if (draft.revision == Doc (s).revision && draft.refs == s.selection.Items () && draft.tool == s.tool)
        return true;
    s.booleanDraft.reset ();
    s.message = "Boolean preview closed: its selection, document or tool changed.";
    return false;
}

void CancelBoolean (Session& s)
{
    s.booleanDraft.reset ();
    s.message.clear ();
}

bool BeginBoolean (Session& s, edit::BooleanKind kind)
{
    if (s.tool != Tool::Select || s.landscapeMode != LandscapeMode::Select || s.edits.InGesture () ||
        GizmoModeLocked (s) || !s.drawing.empty () || s.terrainCapture || s.draftingImport || s.rectangularArray) {
        s.message = "Finish the current input or Esc it, then combine outlines from Select.";
        return false;
    }
    BooleanDraft draft;
    draft.kind = kind;
    draft.refs = s.selection.Items ();
    draft.operands = BooleanOperands (Doc (s), draft.refs);
    if (draft.operands.empty ()) {
        s.message = "Select two or more closed outlines; the first selected is kept, the others are combined with it.";
        return false;
    }
    draft.revision = Doc (s).revision;
    draft.tool = s.tool;
    Preview (Doc (s), draft);
    s.message = draft.error.empty () ? std::string (Title (kind)) + ": Enter applies, Esc cancels." : draft.error;
    s.booleanDraft = std::move (draft);
    return true;
}

bool ApplyBoolean (Session& s)
{
    if (!CheckBooleanDraft (s))
        return false;
    const auto operands = s.booleanDraft->operands;
    const auto kind = s.booleanDraft->kind;
    std::vector<edit::Id> made;
    if (!s.edits.Apply ([&] (edit::Document& doc, std::string& error) {
            return edit::BooleanEntities (doc, operands, kind, error, &made);
        })) {
        // A refusal keeps the draft open, its reason shown, for the user to change the selection.
        s.booleanDraft->error = s.edits.Message ();
        s.message = s.edits.Message ();
        return false;
    }
    std::vector<edit::Ref> refs;
    for (const edit::Id id : made)
        refs.push_back ({ id, edit::Part::Entity, 0 });
    s.selection.Set (refs);
    s.booleanDraft.reset ();
    s.message.clear ();
    return true;
}

void BooleanControls (Session& s)
{
    if (!s.booleanDraft || !CheckBooleanDraft (s))
        return;
    const auto& draft = *s.booleanDraft;
    ImGui::Separator ();
    ImGui::Text ("%s of %zu outlines", Title (draft.kind), draft.operands.size ());
    ImGui::TextDisabled ("The first selected keeps its identity; curves become straight pieces.");
    ImGui::BeginDisabled (!draft.error.empty ());
    if (ImGui::SmallButton ("Apply boolean")) {
        ImGui::EndDisabled ();
        ApplyBoolean (s);
        return;
    }
    ImGui::EndDisabled ();
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Cancel boolean")) {
        CancelBoolean (s);
        return;
    }
    if (!draft.error.empty ())
        ImGui::TextWrapped ("%s", draft.error.c_str ());
}

void DrawBooleanPreview (Session& s, const Map& m, float scale)
{
    if (!CheckBooleanDraft (s))
        return;
    for (const auto& ring : s.booleanDraft->rings)
        for (size_t i = 0; i < ring.size (); ++i)
            Line (s, m, ring[i], ring[(i + 1) % ring.size ()], kAccent, 2.5f * scale);
}

} // namespace geomsrv::archviz::edithud::detail
