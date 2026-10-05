#include "ArchViz/HudMassingRules.hpp"

#include <imgui.h>
#include <algorithm>

namespace geomsrv::archviz::hudmassingrules {

void SyncSite (const std::vector<massingrules::Page>& pages, Draft& active, SiteDraft& site)
{
    if (site.parcels.count (active.source.guid))
        site.parcels[active.source.guid] = active;
    for (auto it = site.parcels.begin (); it != site.parcels.end ();)
        if (std::none_of (pages.begin (), pages.end (), [&] (const auto& page) { return page.guid == it->first; }))
            it = site.parcels.erase (it);
        else
            ++it;
    for (const auto& page : pages)
        Sync (page, site.parcels[page.guid]);
    if (!pages.empty ()) {
        const auto settings = active.calculation;
        const bool labels = active.labels, dimensions = active.offsetDimensions;
        const auto selected = site.parcels.find (active.source.guid);
        active = selected == site.parcels.end () ? site.parcels.at (pages.front ().guid) : selected->second;
        active.calculation = settings;
        active.labels = labels;
        active.offsetDimensions = dimensions;
    }
    else
        Sync ({}, active);
}

massingcalculation::Request SiteInputs (const std::vector<massingrules::Page>& pages, const Draft& active,
                                        const SiteDraft& site)
{
    auto request = active.calculation;
    request.parcels.clear ();
    for (const auto& page : pages) {
        const auto& draft = page.guid == active.source.guid ? active : site.parcels.at (page.guid);
        request.parcels.push_back ({ page, draft.assignments, draft.regulated, draft.endpoints });
    }
    request.before = pages.empty () ? massingrules::Page {} : pages.front ();
    request.assignments.clear ();
    request.regulated.clear ();
    request.endpoints.clear ();
    if (!request.parcels.empty ()) {
        request.assignments = request.parcels.front ().assignments;
        request.regulated = request.parcels.front ().regulated;
        request.endpoints = request.parcels.front ().endpoints;
        if (request.parcels.size () == 1)
            request.parcels.clear ();
    }
    return request;
}

std::vector<massingrules::Edit> DrawSite (const std::vector<massingrules::Page>& pages, Draft& active, SiteDraft& site,
                                          bool busy, const std::string& note,
                                          const std::shared_ptr<const massingcalculation::Preview>& preview)
{
    SyncSite (pages, active, site);
    if (pages.size () > 1) {
        const auto selected = std::find_if (pages.begin (), pages.end (),
                                            [&] (const auto& page) { return page.guid == active.source.guid; });
        int index = int (selected - pages.begin ());
        const auto caption = "Parcel " + std::to_string (index + 1);
        if (ImGui::BeginCombo ("Edit property line", caption.c_str ())) {
            for (size_t i = 0; i < pages.size (); ++i) {
                const auto label = "Parcel " + std::to_string (i + 1) + "##" + pages[i].guid;
                if (ImGui::Selectable (label.c_str (), int (i) == index))
                    active.pickedParcel = pages[i].guid;
            }
            ImGui::EndCombo ();
        }
        ImGui::TextDisabled ("All parcels shown in plan position. Click an outline to edit it.");
        ImGui::TextDisabled ("Terrain and height settings apply to every envelope.");
    }
    const auto sitePreview =
        preview && massingcalculation::SameRequest (preview->inputs, SiteInputs (pages, active, site)) ? preview
                                                                                                       : nullptr;
    auto edits = Draw (active.source, active, busy, note, preview, pages, sitePreview);
    active.calculations.clear (); // Only one complete site request, never an isolated active-parcel solve.
    auto request = SiteInputs (pages, active, site);
    if (!site.lastRequested || !massingcalculation::SameRequest (*site.lastRequested, request)) {
        site.lastRequested = request;
        active.calculations.push_back (std::move (request));
    }
    if (!active.pickedParcel.empty ()) {
        const auto next = site.parcels.find (active.pickedParcel);
        if (next != site.parcels.end () && next->first != active.source.guid) {
            const auto settings = active.calculation;
            const bool labels = active.labels, dimensions = active.offsetDimensions;
            auto calculations = std::move (active.calculations);
            auto numbers = std::move (active.numbers);
            active.pickedParcel.clear ();
            site.parcels[active.source.guid] = active;
            active = next->second;
            active.calculation = settings;
            active.labels = labels;
            active.offsetDimensions = dimensions;
            active.calculations = std::move (calculations);
            // Pending prompts retain their original before GUID and are rejected
            // by AnswerNumber after switching parcels, rather than retargeted.
            active.numbers = std::move (numbers);
        }
        active.pickedParcel.clear ();
    }
    return edits;
}
} // namespace geomsrv::archviz::hudmassingrules
