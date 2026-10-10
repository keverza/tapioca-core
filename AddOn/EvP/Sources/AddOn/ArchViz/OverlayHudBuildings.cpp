#include "ArchViz/OverlayHudEngine.hpp"
#include <cstdio>
#include <ctime>
#include <sstream>

namespace geomsrv::archviz::overlayhud {
namespace {
// Local time to the second: export file names sort by when they were taken.
std::string ExportStamp ()
{
    const std::time_t now = std::time (nullptr);
    std::tm local = {};
    if (localtime_s (&local, &now) != 0)
        return "undated";
    char text[32];
    std::snprintf (text, sizeof (text), "%04d%02d%02d-%02d%02d%02d", local.tm_year + 1900, local.tm_mon + 1,
                   local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
    return text;
}
} // namespace
// The building's highlight, heights and story section, in a section of their own.
void Engine::Impl::StoryDiagram (const massingbuildings::Preview& preview, float ui)
{
    const auto& building = preview.building;
    if (!ImGui::CollapsingHeader ("Story slice editor", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    {
        bool highlighted = store->highlightedBuilding == building.key;
        if (ImGui::Checkbox ("Highlight whole building", &highlighted)) {
            store->highlightedBuilding = highlighted ? building.key : std::string ();
            changes.push_back ({ "buildingHighlight", {}, "Selection", store->highlightedBuilding, -1, 0, {}, true });
        }
    }
    if (store->highlightedBuilding == building.key && !own.massing.inspectionNote.empty ())
        ImGui::TextWrapped ("%s", own.massing.inspectionNote.c_str ());
    for (const auto& page : preview.heights) {
        ImGui::PushID (page.element.c_str ());
        for (auto& edit : hudmeta::Editor (page, *look, ui)) {
            changes.push_back ({ "metadata", {}, "Selection", edit.id, -1, edit.number, edit.text, true });
            store->metadataEdits.push_back (std::move (edit));
        }
        ImGui::PopID ();
    }
    hudsection::Run run = store->pickedFloorBuilding == building.key ? store->floors : hudsection::Run {};
    const auto before = run;
    hudsection::Run hover;
    int pickedStorey = (std::numeric_limits<int>::min) ();
    for (auto& edit : hudsection::Diagram (preview.section, run, *look, ui, store->massingCoefficients, true, &hover,
                                           &pickedStorey)) {
        changes.push_back ({ "metadata", {}, "Selection", edit.id, -1, edit.number, edit.text, true });
        store->metadataEdits.push_back (std::move (edit));
    }
    if (run != before) {
        store->pickedFloorBuilding = building.key;
        store->floors = run;
        changes.push_back ({ "floors", {}, "Selection", building.key, -1, double (run.first), {}, true });
    }
    if (!hover.Empty ()) {
        nextFloorBuilding = building.key;
        nextFloorHover = hover;
    }
    if (pickedStorey != (std::numeric_limits<int>::min) ()) {
        auto& draft = store->buildingPlans[building.key];
        if (draft.story != pickedStorey)
            buildingplan::Cancel (draft);
        draft.story = pickedStorey;
    }
}
// The building's Plan view, a section of its own: its floor among the others at the elevation.
void Engine::Impl::PlanSection (const massingbuildings::Preview& preview, float ui)
{
    const auto& building = preview.building;
    auto& draft = store->buildingPlans[building.key];
    if (!ImGui::CollapsingHeader ("Plan view", ImGuiTreeNodeFlags_DefaultOpen)) {
        buildingplan::Cancel (draft);
        return;
    }
    if (!preview.plan.note.empty ())
        ImGui::TextWrapped ("%s", preview.plan.note.c_str ());
    buildingplan::UseProgramme (draft, store->massingProgramme);
    // Every building is the floor's context; one not yet in the site's plans stands alone.
    std::map<std::string, buildingplan::Plan> alone;
    const auto& plans = store->floorPlanSnapshots.contains (building.key) ? store->floorPlanSnapshots
                                                                          : (alone[building.key] = preview.plan, alone);
    auto asked =
        hudfloorscheme::PlanView (store->planEditors[building.key], store->floorPlanner, plans, store->buildingPlans,
                                  building.key, store->massingProgramme, store->massingCoefficients, ui);
    for (auto& edit : asked.edits) {
        changes.push_back ({ "metadata", {}, "Selection", edit.id, -1, 0, {}, true });
        store->metadataEdits.push_back (std::move (edit));
    }
    if (asked.exportPlan)
        if (const auto* floor = buildingplan::Displayed (preview.plan, draft)) {
            std::map<int, const floorscheme::Scheme*> schemes;
            for (const auto& f : preview.plan.floors)
                if (const auto* planned = store->floorPlanner.Latest (buildingplan::FloorId (building.key, f.story)))
                    schemes[f.story] = &planned->scheme;
            store->planExports.push_back (
                buildingplan::ExportPlan (preview.plan, draft, *floor, ExportStamp (), schemes));
            changes.push_back ({ "planExport", {}, "Selection", building.key, -1, 1, {}, true });
        }
    // A new scheme, a changed design or selection: the overlay draws the floors again.
    std::ostringstream signature;
    signature << store->floorPlanner.Revision () << ':' << floorscheme::edit::ToJson (draft.designs) << ':'
              << draft.cores.size ();
    if (const auto selected = hudfloorscheme::Selected (store->planEditors[building.key]))
        signature << ':' << selected->story << ':' << selected->flat.front ().x << ',' << selected->flat.front ().y;
    if (signature.str () != draft.previewFingerprint) {
        draft.previewFingerprint = signature.str ();
        changes.push_back ({ "massingPlanPreview", {}, "Selection", "local floor design", -1, 1, {}, true });
    }
}
void Engine::Impl::BuildingDiagram (const massingbuildings::Preview& preview, float ui)
{
    const auto& building = preview.building;
    ImGui::PushID (building.key.c_str ());
    if (building.id.empty ())
        ImGui::TextDisabled ("Unassigned source (no building ID)");
    else
        ImGui::TextWrapped ("Building %s (%zu sources)", building.id.c_str (), building.guids.size ());
    if (!preview.section.note.empty ())
        ImGui::TextWrapped ("%s", preview.section.note.c_str ());
    StoryDiagram (preview, ui);
    PlanSection (preview, ui);
    ImGui::PopID ();
}
} // namespace geomsrv::archviz::overlayhud
