#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/HudShell.hpp"
#include "Edit/EditOrganize.hpp"
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <functional>

// The virtual editor's layers and groups palette, and the hide, lock, isolate and selection
// commands the panel, the menu and the keys share (EditOrganize.hpp does the work).
namespace geomsrv::archviz::edithud::detail {

namespace {
// The colours a layer can take; 0 is the editor's own line colour.
constexpr uint32_t kLayerColours[] = { 0u,          0xD9480FFFu, 0x2F9E44FFu, 0x1971C2FFu, 0x9C36B5FFu,
                                       0xF08C00FFu, 0x0C8599FFu, 0x495057FFu, 0xC2255CFFu };

ImU32 Colour (uint32_t rgba)
{
    return IM_COL32 ((rgba >> 24) & 0xFF, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF, rgba & 0xFF);
}

// One organisation edit as one undo step; the selection then lets go of what it hid or locked.
void Change (Session& s, const edit::Operation& op)
{
    if (s.drag != Drag::None || !s.drawing.empty ()) {
        s.message = "Finish the current input or Esc it first.";
        return;
    }
    if (!s.edits.Apply (op))
        s.message = s.edits.Message ();
    s.selection.Prune (Doc (s));
}

void ChangeLayer (Session& s, edit::Id id, std::function<void (edit::Layer&)> change)
{
    Change (s, [id, change] (edit::Document& d, std::string& error) {
        edit::Layer* l = edit::FindLayer (d, id);
        if (l == nullptr) {
            error = "the layer no longer exists";
            return false;
        }
        change (*l);
        return true;
    });
}

std::string Label (const edit::Document& doc, edit::Id id)
{
    std::string kind = "Path";
    std::string role;
    if (const edit::Entity* e = edit::Find (doc, id)) {
        kind = e->kind == edit::Kind::Extrusion ? "Extrusion" : e->closed ? "Outline" : "Line";
        role = e->role;
    }
    else if (const edit::Path* p = edit::FindPath (doc, id))
        role = p->role;
    return kind + (role.empty () ? "" : " " + role) + "  #" + std::to_string (id);
}

// An object's row: shown and locked toggles, then its name, which selects it (Ctrl adds).
void ObjectRow (Session& s, edit::Id id)
{
    const edit::Membership m = *edit::MemberOf (Doc (s), id);
    const std::string label = Label (Doc (s), id);
    const bool pickable = edit::Pickable (Doc (s), id);
    const std::vector<edit::Ref> ref { { id, edit::Part::Entity, edit::kNoId } };
    ImGui::PushID (int (id));
    bool shown = !m.hidden, locked = m.locked;
    if (ImGui::Checkbox ("##shown", &shown))
        Change (s, [ref, shown] (edit::Document& d, std::string& e) { return edit::SetHidden (d, ref, !shown, e); });
    hudshell::Tip ("Shown", hudshell::TipSide::Above);
    ImGui::SameLine ();
    if (ImGui::Checkbox ("##locked", &locked))
        Change (s, [ref, locked] (edit::Document& d, std::string& e) { return edit::SetLocked (d, ref, locked, e); });
    hudshell::Tip ("Locked: drawn and snapped to, not picked", hudshell::TipSide::Above);
    ImGui::SameLine ();
    const bool selected = s.selection.Contains (ref.front ());
    ImGui::BeginDisabled (!pickable);
    if (ImGui::Selectable (label.c_str (), selected)) {
        if (ImGui::GetIO ().KeyCtrl)
            s.selection.Toggle (ref.front ());
        else
            s.selection.Set (ref);
    }
    ImGui::EndDisabled ();
    ImGui::PopID ();
}

// What sits directly in `group` (kNoId: in none), read before any row edits the document: an
// edit replaces it, so nothing is iterated in place.
void Contents (const edit::Document& doc, edit::Id group, std::vector<edit::Id>& groups, std::vector<edit::Id>& objects)
{
    for (const auto& g : doc.groups)
        if (g.parent == group)
            groups.push_back (g.id);
    for (const auto& p : doc.paths)
        if (p.member.group == group)
            objects.push_back (p.id);
    for (const auto& e : doc.entities)
        if (e.member.group == group)
            objects.push_back (e.id);
}

void GroupTree (Session& s, edit::Id group)
{
    const edit::Group* g = edit::FindGroup (Doc (s), group);
    if (g == nullptr)
        return; // gone with an edit made earlier this frame
    ImGui::PushID (int (group));
    const bool open = ImGui::TreeNodeEx ("##group", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", g->name.c_str ());
    if (ImGui::IsItemClicked () && !ImGui::IsItemToggledOpen ()) {
        std::vector<edit::Ref> members;
        for (const edit::Id id : edit::GroupMembers (Doc (s), group))
            if (edit::Pickable (Doc (s), id))
                members.push_back ({ id, edit::Part::Entity, edit::kNoId });
        s.selection.Set (members);
    }
    if (open) {
        std::vector<edit::Id> groups, objects;
        Contents (Doc (s), group, groups, objects);
        for (const edit::Id child : groups)
            GroupTree (s, child);
        for (const edit::Id id : objects)
            if (edit::MemberOf (Doc (s), id) != nullptr)
                ObjectRow (s, id);
        ImGui::TreePop ();
    }
    ImGui::PopID ();
}
} // namespace

void Organize (Session& s, OrganizeCommand command)
{
    const auto refs = s.selection.Items ();
    const auto& doc = Doc (s);
    switch (command) {
        case OrganizeCommand::Hide:
            Change (s, [refs] (edit::Document& d, std::string& e) { return edit::SetHidden (d, refs, true, e); });
            break;
        case OrganizeCommand::Lock:
            Change (s, [refs] (edit::Document& d, std::string& e) { return edit::SetLocked (d, refs, true, e); });
            break;
        case OrganizeCommand::Isolate:
            Change (s, [refs] (edit::Document& d, std::string& e) { return edit::Isolate (d, refs, e); });
            break;
        case OrganizeCommand::ShowIsolated:
            Change (s, [] (edit::Document& d, std::string& e) {
                if (!edit::ShowIsolated (d))
                    e = "nothing is isolated";
                return e.empty ();
            });
            break;
        case OrganizeCommand::ShowAll:
            Change (s, [] (edit::Document& d, std::string& e) {
                if (!edit::ShowAll (d))
                    e = "nothing is hidden";
                return e.empty ();
            });
            break;
        case OrganizeCommand::UnlockAll:
            Change (s, [] (edit::Document& d, std::string& e) {
                if (!edit::UnlockAll (d))
                    e = "nothing is locked";
                return e.empty ();
            });
            break;
        case OrganizeCommand::SelectAll:
            s.selection.Set (edit::SelectAll (doc));
            break;
        case OrganizeCommand::Invert:
            s.selection.Set (edit::Inverse (doc, refs));
            break;
        case OrganizeCommand::SameRole:
        case OrganizeCommand::SameLayer:
        case OrganizeCommand::SameKind:
            s.selection.Set (edit::SelectLike (doc, refs,
                                               command == OrganizeCommand::SameRole    ? edit::Likeness::Role
                                               : command == OrganizeCommand::SameLayer ? edit::Likeness::Layer
                                                                                       : edit::Likeness::Kind));
            break;
    }
}

void OrganizeControls (Session& s)
{
    // Open of itself once there is something to organise; the user's choice after that.
    if (!Doc (s).layers.empty () || !Doc (s).groups.empty ())
        ImGui::SetNextItemOpen (true, ImGuiCond_Once);
    if (!ImGui::TreeNode ("Layers and groups"))
        return;
    const bool any = !s.selection.Items ().empty ();
    const bool isolated = !Doc (s).isolated.empty ();
    ImGui::BeginDisabled (s.drag != Drag::None || !s.drawing.empty ());
    ImGui::BeginDisabled (!any);
    if (ImGui::SmallButton ("Hide"))
        Organize (s, OrganizeCommand::Hide);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Lock"))
        Organize (s, OrganizeCommand::Lock);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Isolate"))
        Organize (s, OrganizeCommand::Isolate);
    ImGui::EndDisabled ();
    ImGui::SameLine ();
    ImGui::BeginDisabled (!isolated);
    if (ImGui::SmallButton ("Show isolated"))
        Organize (s, OrganizeCommand::ShowIsolated);
    ImGui::EndDisabled ();
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Show all"))
        Organize (s, OrganizeCommand::ShowAll);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Unlock all"))
        Organize (s, OrganizeCommand::UnlockAll);

    if (ImGui::SmallButton ("Select all"))
        Organize (s, OrganizeCommand::SelectAll);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Invert"))
        Organize (s, OrganizeCommand::Invert);
    ImGui::BeginDisabled (!any);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Same role"))
        Organize (s, OrganizeCommand::SameRole);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Same layer"))
        Organize (s, OrganizeCommand::SameLayer);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Same kind"))
        Organize (s, OrganizeCommand::SameKind);
    ImGui::EndDisabled ();

    // Layers: shown, locked, colour, name; the selection moves onto one, or off every layer.
    ImGui::SeparatorText ("Layers");
    const auto refs = s.selection.Items ();
    const std::vector<edit::Layer> layers = Doc (s).layers; // a row's edit replaces the document
    for (const auto& layer : layers) {
        const edit::Id id = layer.id;
        ImGui::PushID (int (id));
        bool visible = layer.visible, locked = layer.locked;
        if (ImGui::Checkbox ("##visible", &visible))
            ChangeLayer (s, id, [visible] (edit::Layer& l) { l.visible = visible; });
        hudshell::Tip ("Shown", hudshell::TipSide::Above);
        ImGui::SameLine ();
        if (ImGui::Checkbox ("##locked", &locked))
            ChangeLayer (s, id, [locked] (edit::Layer& l) { l.locked = locked; });
        hudshell::Tip ("Locked: drawn and snapped to, not picked", hudshell::TipSide::Above);
        ImGui::SameLine ();
        const uint32_t own = layer.rgba;
        if (ImGui::ColorButton ("##colour", ImGui::ColorConvertU32ToFloat4 (Colour (own != 0 ? own : 0x2B8FE0FFu))))
            ImGui::OpenPopup ("colours");
        if (ImGui::BeginPopup ("colours")) {
            for (const uint32_t c : kLayerColours) {
                ImGui::PushID (int (c));
                if (ImGui::ColorButton (c == 0 ? "Editor colour" : "##c",
                                        ImGui::ColorConvertU32ToFloat4 (Colour (c != 0 ? c : 0x2B8FE0FFu)))) {
                    ChangeLayer (s, id, [c] (edit::Layer& l) { l.rgba = c; });
                    ImGui::CloseCurrentPopup ();
                }
                ImGui::SameLine ();
                ImGui::PopID ();
            }
            ImGui::NewLine ();
            ImGui::EndPopup ();
        }
        ImGui::SameLine ();
        char name[64];
        std::snprintf (name, sizeof name, "%s", layer.name.c_str ());
        ImGui::SetNextItemWidth (120.0f);
        if (ImGui::InputText ("##name", name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue) &&
            layer.name != name) {
            const std::string renamed = name;
            ChangeLayer (s, id, [renamed] (edit::Layer& l) { l.name = renamed; });
        }
        ImGui::SameLine ();
        ImGui::BeginDisabled (refs.empty ());
        if (ImGui::SmallButton ("Move here"))
            Change (s, [refs, id] (edit::Document& d, std::string& e) { return edit::MoveToLayer (d, refs, id, e); });
        ImGui::EndDisabled ();
        ImGui::SameLine ();
        if (ImGui::SmallButton ("Delete"))
            Change (s, [id] (edit::Document& d, std::string& e) { return edit::DeleteLayer (d, id, e); });
        ImGui::PopID ();
    }
    if (ImGui::SmallButton ("New layer")) {
        const std::string name = "Layer " + std::to_string (layers.size () + 1);
        Change (s, [refs, name] (edit::Document& d, std::string&) {
            const edit::Id id = edit::AddLayer (d, name);
            std::string unused;
            if (!refs.empty ())
                edit::MoveToLayer (d, refs, id, unused); // what is selected goes onto it
            return true;
        });
    }
    hudshell::Tip ("A new layer, with the selection moved onto it", hudshell::TipSide::Above);
    ImGui::SameLine ();
    ImGui::BeginDisabled (refs.empty ());
    if (ImGui::SmallButton ("Off layers"))
        Change (s, [refs] (edit::Document& d, std::string& e) { return edit::MoveToLayer (d, refs, edit::kNoId, e); });
    ImGui::EndDisabled ();

    // Objects, by group. A group's name selects all of it; an object's, itself.
    ImGui::SeparatorText ("Objects");
    std::vector<edit::Id> groups, objects;
    Contents (Doc (s), edit::kNoId, groups, objects);
    for (const edit::Id g : groups)
        GroupTree (s, g);
    for (const edit::Id id : objects)
        if (edit::MemberOf (Doc (s), id) != nullptr)
            ObjectRow (s, id);
    ImGui::EndDisabled ();
    ImGui::TreePop ();
}

} // namespace geomsrv::archviz::edithud::detail
