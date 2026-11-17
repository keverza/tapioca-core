#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/HudShell.hpp"
#include "Edit/EditOrganize.hpp"
#include <imgui.h>
#include <cctype>
#include <cfloat>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <optional>
#include <utility>

// The virtual editor's layers and groups palette, and the hide, lock, isolate and selection
// commands the panel, the menu and the keys share (EditOrganize.hpp does the work).
namespace geomsrv::archviz::edithud::detail {

namespace {
// The editor's own line colour (editlayers::Style::geometry), which a layer without one draws in.
constexpr uint32_t kEditorLine = 0x2B8FE0FFu;

ImVec4 Float4 (uint32_t rgba)
{
    return { float ((rgba >> 24) & 0xFF) / 255.0f, float ((rgba >> 16) & 0xFF) / 255.0f,
             float ((rgba >> 8) & 0xFF) / 255.0f, float (rgba & 0xFF) / 255.0f };
}

uint32_t Pack (const float c[4])
{
    uint32_t rgba = 0;
    for (int i = 0; i < 4; ++i)
        rgba |= uint32_t (std::clamp (c[i], 0.0f, 1.0f) * 255.0f + 0.5f) << (24 - i * 8);
    return rgba;
}

std::string Hex (uint32_t rgba)
{
    char text[16];
    std::snprintf (text, sizeof text, "#%06X", unsigned (rgba >> 8));
    return text;
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

// An object's row: shown and locked toggles, then its name, which selects it (Ctrl adds). Hovered,
// it is what the view highlights; selected in the view, it is scrolled into sight once (#191 U5).
void ObjectRow (Session& s, edit::Id id, bool reveal = false)
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
    if (ImGui::IsItemHovered (ImGuiHoveredFlags_AllowWhenDisabled))
        s.outlinerHover = ref.front ();
    if (ImGui::BeginPopupContextItem ("object")) { // #124
        if (ImGui::MenuItem ("Focus in view"))
            FocusOn (s, { ref.front ().entity }, label);
        ImGui::EndPopup ();
    }
    if (reveal && selected)
        ImGui::SetScrollHereY (0.5f);
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

// #124: the last item a drag source (carrying `id`) and a drop target for rows of the same kind.
template <typename Drop> void DragRow (const char* payload, edit::Id id, const std::string& name, Drop drop)
{
    if (ImGui::BeginDragDropSource ()) {
        ImGui::SetDragDropPayload (payload, &id, sizeof id);
        ImGui::Text ("Move %s", name.c_str ());
        ImGui::EndDragDropSource ();
    }
    if (ImGui::BeginDragDropTarget ()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload (payload); p != nullptr && p->DataSize == sizeof id) {
            edit::Id moved = edit::kNoId;
            std::memcpy (&moved, p->Data, sizeof moved);
            drop (moved);
        }
        ImGui::EndDragDropTarget ();
    }
}
constexpr const char* kLayerPayload = "TAPIOCA_LAYER_ROW";
constexpr const char* kGroupPayload = "TAPIOCA_GROUP_ROW";

void GroupTree (Session& s, edit::Id group, bool reveal)
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
    DragRow (kGroupPayload, group, g->name, [&s, group] (edit::Id moved) { DropGroup (s, moved, group); });
    if (ImGui::IsItemHovered () && !ImGui::IsPopupOpen ("rename") && ImGui::GetDragDropPayload () == nullptr)
        hudshell::HoverTip ("Click selects the group. Drag onto a group beside it to move it there. Right-click to "
                            "focus on it or rename it.");
    // #124: a group renamed in place, one undo step; a refused name says why and the box stays.
    if (ImGui::BeginPopupContextItem ("rename")) {
        static char name[64]; // the box's draft: one popup is open at a time
        const bool appearing = ImGui::IsWindowAppearing ();
        if (appearing)
            std::snprintf (name, sizeof name, "%s", g->name.c_str ());
        if (ImGui::MenuItem ("Focus in view")) {
            FocusOn (s, edit::GroupMembers (Doc (s), group), "group " + g->name);
            ImGui::CloseCurrentPopup ();
        }
        ImGui::Separator ();
        ImGui::TextUnformatted ("Rename group");
        if (appearing)
            ImGui::SetKeyboardFocusHere (); // typing goes straight into the name
        if (ImGui::InputText ("##name", name, sizeof name,
                              ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            if (RenameGroup (s, group, name) || s.message.empty ())
                ImGui::CloseCurrentPopup ();
            else
                ImGui::SetKeyboardFocusHere (-1); // back into the box, its text selected, for another try
        }
        ImGui::EndPopup ();
    }
    if (open) {
        std::vector<edit::Id> groups, objects;
        Contents (Doc (s), group, groups, objects);
        for (const edit::Id child : groups)
            GroupTree (s, child, reveal);
        for (const edit::Id id : objects)
            if (edit::MemberOf (Doc (s), id) != nullptr)
                ObjectRow (s, id, reveal);
        ImGui::TreePop ();
    }
    ImGui::PopID ();
}
} // namespace

bool SwatchPicker (const char* id, uint32_t& rgba, uint32_t editorColour)
{
    ImGui::PushID (id);
    const uint32_t shown = rgba != 0 || editorColour == 0 ? rgba : editorColour;
    static float custom[4]; // the picker's draft: one popup is open at a time
    bool changed = false;
    if (ImGui::ColorButton ("##swatch", Float4 (shown))) {
        const ImVec4 c = Float4 (shown);
        custom[0] = c.x, custom[1] = c.y, custom[2] = c.z, custom[3] = c.w;
        ImGui::OpenPopup ("swatches");
    }
    if (ImGui::BeginPopup ("swatches")) {
        const auto choose = [&] (uint32_t value) {
            rgba = value;
            changed = true;
            ImGui::CloseCurrentPopup ();
        };
        if (editorColour != 0) {
            if (ImGui::ColorButton ("The editor's own colour", Float4 (editorColour)))
                choose (0);
            ImGui::SameLine ();
            ImGui::TextDisabled ("The editor's own");
        }
        for (size_t i = 0; i < std::size (kSwatches); ++i) {
            if (i % 11 != 0)
                ImGui::SameLine ();
            ImGui::PushID (int (i));
            if (ImGui::ColorButton (Hex (kSwatches[i]).c_str (), Float4 (kSwatches[i])))
                choose (kSwatches[i]);
            ImGui::PopID ();
        }
        ImGui::SeparatorText ("Your own");
        ImGui::SetNextItemWidth (200.0f);
        ImGui::ColorPicker4 ("##own", custom, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_AlphaBar);
        if (ImGui::SmallButton ("Use this colour"))
            choose (Pack (custom));
        ImGui::EndPopup ();
    }
    ImGui::PopID ();
    return changed;
}

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

void OrganizeVisibility (Session& s)
{
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
    ImGui::EndDisabled ();
}

void SelectMenuItems (Session& s)
{
    const bool any = !s.selection.Items ().empty ();
    const bool idle = s.drag == Drag::None && s.drawing.empty ();
    if (ImGui::MenuItem ("Select all", nullptr, false, idle))
        Organize (s, OrganizeCommand::SelectAll);
    if (ImGui::MenuItem ("Invert", nullptr, false, idle))
        Organize (s, OrganizeCommand::Invert);
    if (ImGui::MenuItem ("Same role", nullptr, false, idle && any))
        Organize (s, OrganizeCommand::SameRole);
    if (ImGui::MenuItem ("Same layer", nullptr, false, idle && any))
        Organize (s, OrganizeCommand::SameLayer);
    if (ImGui::MenuItem ("Same kind", nullptr, false, idle && any))
        Organize (s, OrganizeCommand::SameKind);
}

bool FocusOn (Session& s, const std::vector<edit::Id>& objects, const std::string& what)
{
    std::vector<edit::Ref> shown;
    for (const edit::Id id : objects)
        if (edit::Shown (Doc (s), id))
            shown.push_back ({ id, edit::Part::Entity, edit::kNoId });
    edit::Frame box; // world axes
    if (shown.empty () || !edit::FitBox (Doc (s), shown, box)) {
        s.message = objects.empty () ? "Nothing to focus on: " + what + " is empty."
                                     : "Nothing to focus on: everything in " + what + " is hidden.";
        return false;
    }
    // A margin round it, and never less than a few metres across: a point or a short line is
    // framed with its surroundings, not filling the view.
    Session::FocusRequest request;
    request.what = what;
    for (int a = 0; a < 3; ++a) {
        const double half = std::max (box.half[a] * 1.15, a < 2 ? 2.5 : 0.0);
        const double c = a == 0 ? box.origin.x : a == 1 ? box.origin.y : box.origin.z;
        (a == 0 ? request.lo.x : a == 1 ? request.lo.y : request.lo.z) = c - half;
        (a == 0 ? request.hi.x : a == 1 ? request.hi.y : request.hi.z) = c + half;
    }
    s.focusPending = request;
    s.focusRequest.reset (); // the newest ask wins
    s.message.clear ();
    return true;
}

void TakeFocus (Session& s, const Map& m)
{
    if (!s.focusPending || s.focusQueued)
        return;
    Session::FocusRequest request = *std::exchange (s.focusPending, std::nullopt);
    request.planar = m.v.planar;
    if (!request.planar) {
        float rect[4] = { FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
        bool any = false;
        for (int corner = 0; corner < 8; ++corner) {
            const edit::Vec3 p { corner & 1 ? request.hi.x : request.lo.x, corner & 2 ? request.hi.y : request.lo.y,
                                 corner & 4 ? request.hi.z : request.lo.z };
            ImVec2 at;
            if (!m.Screen (p, at))
                continue;
            any = true;
            rect[0] = std::min (rect[0], at.x), rect[1] = std::min (rect[1], at.y);
            rect[2] = std::max (rect[2], at.x), rect[3] = std::max (rect[3], at.y);
        }
        // Behind the camera: no rectangle. Archicad's 3D zoom needs one and refuses; the Tapioca
        // viewer frames the box itself and does not (FollowFocus).
        request.onScreen = any && rect[2] - rect[0] >= 1.0f && rect[3] - rect[1] >= 1.0f;
        if (request.onScreen)
            std::copy (std::begin (rect), std::end (rect), request.rect);
    }
    s.focusRequest = std::move (request);
}

bool DropLayer (Session& s, edit::Id layer, edit::Id onto)
{
    const uint64_t revision = Doc (s).revision;
    s.message.clear ();
    Change (s, [layer, onto] (edit::Document& d, std::string& e) { return edit::MoveLayer (d, layer, onto, e); });
    return Doc (s).revision != revision;
}

bool DropGroup (Session& s, edit::Id group, edit::Id onto)
{
    const uint64_t revision = Doc (s).revision;
    s.message.clear ();
    Change (s, [group, onto] (edit::Document& d, std::string& e) { return edit::MoveGroup (d, group, onto, e); });
    return Doc (s).revision != revision;
}

bool RenameGroup (Session& s, edit::Id group, const std::string& name)
{
    const uint64_t revision = Doc (s).revision;
    s.message.clear ();
    Change (s, [group, name] (edit::Document& d, std::string& e) { return edit::RenameGroup (d, group, name, e); });
    return Doc (s).revision != revision;
}

void LayerRows (Session& s)
{
    ImGui::BeginDisabled (s.drag != Drag::None || !s.drawing.empty ());
    // Layers, 0 first: shown, locked, colour, name; the selection moves onto one. They are the
    // editor's alone: none of them is an Archicad layer.
    // Under the Outliner's Layers header (#191 U5): no second title.
    const auto refs = s.selection.Items ();
    std::vector<edit::Layer> layers { Doc (s).layer0 }; // copies: a row's edit replaces the document
    layers.insert (layers.end (), Doc (s).layers.begin (), Doc (s).layers.end ());
    for (const auto& layer : layers) {
        const edit::Id id = layer.id;
        ImGui::PushID (int (id));
        // #124: the grip to drag the row by (the name is a text box); layer 0 stays first.
        ImGui::BeginDisabled (id == edit::kNoId);
        ImGui::SmallButton ("=");
        ImGui::EndDisabled ();
        if (id != edit::kNoId) {
            DragRow (kLayerPayload, id, layer.name, [&s, id] (edit::Id moved) { DropLayer (s, moved, id); });
            if (ImGui::IsItemHovered () && ImGui::GetDragDropPayload () == nullptr)
                hudshell::HoverTip ("Drag onto another layer to move it there");
        }
        ImGui::SameLine ();
        bool visible = layer.visible, locked = layer.locked;
        if (ImGui::Checkbox ("##visible", &visible))
            ChangeLayer (s, id, [visible] (edit::Layer& l) { l.visible = visible; });
        hudshell::Tip ("Shown", hudshell::TipSide::Above);
        ImGui::SameLine ();
        if (ImGui::Checkbox ("##locked", &locked))
            ChangeLayer (s, id, [locked] (edit::Layer& l) { l.locked = locked; });
        hudshell::Tip ("Locked: drawn and snapped to, not picked", hudshell::TipSide::Above);
        ImGui::SameLine ();
        uint32_t colour = layer.rgba;
        if (SwatchPicker ("colour", colour, kEditorLine))
            ChangeLayer (s, id, [colour] (edit::Layer& l) { l.rgba = colour; });
        ImGui::SameLine ();
        char name[64];
        std::snprintf (name, sizeof name, "%s", layer.name.c_str ());
        ImGui::SetNextItemWidth (120.0f);
        ImGui::BeginDisabled (id == edit::kNoId); // layer 0 keeps its name
        if (ImGui::InputText ("##name", name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue) &&
            layer.name != name) {
            const std::string renamed = name; // #124: refused (empty, taken) with the reason
            Change (
                s, [id, renamed] (edit::Document& d, std::string& e) { return edit::RenameLayer (d, id, renamed, e); });
        }
        ImGui::EndDisabled ();
        ImGui::SameLine ();
        ImGui::BeginDisabled (refs.empty ());
        if (ImGui::SmallButton ("Move here"))
            Change (s, [refs, id] (edit::Document& d, std::string& e) { return edit::MoveToLayer (d, refs, id, e); });
        ImGui::EndDisabled ();
        ImGui::SameLine ();
        if (ImGui::SmallButton ("Focus")) { // #124: frame what is shown on it; the document is untouched
            std::vector<edit::Id> on;
            for (const auto& p : Doc (s).paths)
                if (p.member.layer == id)
                    on.push_back (p.id);
            for (const auto& e : Doc (s).entities)
                if (e.member.layer == id)
                    on.push_back (e.id);
            FocusOn (s, on, "layer " + layer.name);
        }
        hudshell::Tip ("Frame what is shown on this layer in the view", hudshell::TipSide::Above);
        if (id != edit::kNoId) {
            ImGui::SameLine ();
            if (ImGui::SmallButton ("Delete"))
                Change (s, [id] (edit::Document& d, std::string& e) { return edit::DeleteLayer (d, id, e); });
            hudshell::Tip ("Its objects go to layer 0", hudshell::TipSide::Above);
        }
        ImGui::PopID ();
    }
    if (ImGui::SmallButton ("New layer")) {
        // Named and coloured in turn: each new layer takes the next swatch.
        const uint32_t colour = kSwatches[(layers.size () - 1) % std::size (kSwatches)];
        Change (s, [refs, colour] (edit::Document& d, std::string&) {
            const edit::Id id = edit::AddLayer (d, {}); // the first free "Layer N"
            edit::FindLayer (d, id)->rgba = colour;
            std::string unused;
            if (!refs.empty ())
                edit::MoveToLayer (d, refs, id, unused); // what is selected goes onto it
            return true;
        });
    }
    hudshell::Tip ("A new layer, with the selection moved onto it", hudshell::TipSide::Above);

    ImGui::EndDisabled ();
}

void LayerAndGroupRow (Session& s)
{
    const auto refs = s.selection.Items ();
    // The layer the selection shares, or none when it is on several.
    std::optional<edit::Id> shared;
    bool mixed = false;
    for (const auto& ref : refs)
        if (const auto* m = edit::MemberOf (Doc (s), ref.entity)) {
            mixed = mixed || (shared && *shared != m->layer);
            shared = m->layer;
        }
    std::vector<edit::Layer> layers { Doc (s).layer0 };
    layers.insert (layers.end (), Doc (s).layers.begin (), Doc (s).layers.end ());
    std::string current = mixed ? "(several)" : "";
    for (const auto& layer : layers)
        if (!mixed && shared && layer.id == *shared)
            current = layer.name;
    ImGui::BeginDisabled (s.drag != Drag::None || !s.drawing.empty () || refs.empty ());
    ImGui::SetNextItemWidth (120.0f);
    if (ImGui::BeginCombo ("Layer", current.c_str ())) {
        for (const auto& layer : layers)
            if (ImGui::Selectable (layer.name.c_str (), !mixed && shared && layer.id == *shared)) {
                const edit::Id id = layer.id;
                Change (s,
                        [refs, id] (edit::Document& d, std::string& e) { return edit::MoveToLayer (d, refs, id, e); });
            }
        ImGui::EndCombo ();
    }
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Group"))
        GroupSelection (s, true);
    ImGui::SameLine ();
    if (ImGui::SmallButton ("Ungroup"))
        GroupSelection (s, false);
    ImGui::EndDisabled ();
}

void ObjectRows (Session& s, const std::string& filter, bool reveal)
{
    ImGui::BeginDisabled (s.drag != Drag::None || !s.drawing.empty ());
    if (!filter.empty ()) {
        // Searching: every object whose row reads the text, flat, whatever group it is in.
        const auto lower = [] (std::string text) {
            for (auto& c : text)
                c = char (std::tolower (static_cast<unsigned char> (c)));
            return text;
        };
        const auto wanted = lower (filter);
        std::vector<edit::Id> ids;
        for (const auto& p : Doc (s).paths)
            ids.push_back (p.id);
        for (const auto& e : Doc (s).entities)
            ids.push_back (e.id);
        size_t shown = 0;
        for (const edit::Id id : ids)
            if (edit::MemberOf (Doc (s), id) != nullptr &&
                lower (Label (Doc (s), id)).find (wanted) != std::string::npos) {
                ObjectRow (s, id, reveal);
                ++shown;
            }
        if (!shown)
            ImGui::TextDisabled ("No object matches \"%s\".", filter.c_str ());
        ImGui::EndDisabled ();
        return;
    }
    // Objects, by group. A group's name selects all of it; an object's, itself.
    std::vector<edit::Id> groups, objects;
    Contents (Doc (s), edit::kNoId, groups, objects);
    for (const edit::Id g : groups)
        GroupTree (s, g, reveal);
    for (const edit::Id id : objects)
        if (edit::MemberOf (Doc (s), id) != nullptr)
            ObjectRow (s, id, reveal);
    if (groups.empty () && objects.empty ())
        ImGui::TextDisabled ("No objects yet.");
    ImGui::EndDisabled ();
}

} // namespace geomsrv::archviz::edithud::detail
