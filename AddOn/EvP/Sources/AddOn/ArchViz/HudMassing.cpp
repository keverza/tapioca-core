#include "ArchViz/HudMassing.hpp"
#include "ArchViz/HudShell.hpp"

#include <imgui.h>
#include <set>

namespace geomsrv::archviz::hudmassing {

const char* Role (Group group)
{
    switch (group) {
        case Group::PropertyLine:
            return "PropertyLine";
        case Group::ExistingTerrain:
            return "ExistingTerrain";
        case Group::NewTerrain:
            return "NewTerrain";
        case Group::MassingSlabs:
            return "MassingSlab";
    }
    return "";
}

const char* Label (Group group)
{
    switch (group) {
        case Group::PropertyLine:
            return "Property line";
        case Group::ExistingTerrain:
            return "Existing terrain";
        case Group::NewTerrain:
            return "New terrain";
        case Group::MassingSlabs:
            return "Massing slabs";
    }
    return "";
}

std::vector<Assignment> Plan (Request request, const std::vector<std::string>& current,
                              const std::vector<std::string>& selected)
{
    const std::set<std::string> before (current.begin (), current.end ());
    const std::set<std::string> selection (selected.begin (), selected.end ());
    std::set<std::string> after = before;
    switch (request.action) {
        case Action::Reselect:
            return {};
        case Action::Clear:
            after.clear ();
            break;
        case Action::Update:
            after = selection;
            break;
        case Action::Add:
            after.insert (selection.begin (), selection.end ());
            break;
        case Action::Remove:
            for (const auto& guid : selection)
                after.erase (guid);
            break;
    }
    std::vector<Assignment> writes;
    for (const auto& guid : before)
        if (!after.count (guid))
            writes.push_back ({ guid, false });
    for (const auto& guid : after)
        if (!before.count (guid))
            writes.push_back ({ guid, true });
    return writes;
}

std::vector<Request> Draw (const Page& page)
{
    std::vector<Request> requests;
    if (ImGui::CollapsingHeader ("Define", ImGuiTreeNodeFlags_DefaultOpen)) {
        constexpr const char* actions[] = { "Update", "Add", "Remove", "Reselect", "Clear" };
        constexpr const char* tips[] = { "Replace this group with the viewport selection", "Add the viewport selection",
                                         "Remove selected members from this group",
                                         "Select this group's saved elements in the viewport",
                                         "Clear this group's role assignments (keep other metadata)" };
        for (size_t i = 0; i < page.guids.size (); ++i) {
            const Group group = Group (i);
            ImGui::PushID (int (i));
            ImGui::Text ("%s (%zu)", Label (group), page.guids[i].size ());
            ImGui::BeginDisabled (!page.known);
            for (int a = 0; a < 5; ++a) {
                if (a != 0)
                    ImGui::SameLine ();
                if (ImGui::SmallButton (actions[a]))
                    requests.push_back ({ group, Action (a) });
                hudshell::Tip (tips[a]);
            }
            ImGui::EndDisabled ();
            ImGui::PopID ();
        }
    }
    if (!page.note.empty ())
        ImGui::TextWrapped ("%s", page.note.c_str ());
    ImGui::TextDisabled ("Roles are stored in element user information.");
    return requests;
}

} // namespace geomsrv::archviz::hudmassing
