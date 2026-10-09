#include "ArchViz/OverlayHudEngine.hpp"
#include <sstream>

namespace geomsrv::archviz::overlayhud {
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
    buildingplan::UseProgramme (store->buildingPlans[building.key], store->massingProgramme);
    for (auto& edit :
         buildingplan::Draw (preview.plan, store->buildingPlans[building.key], ui, store->massingCoefficients)) {
        changes.push_back ({ "metadata", {}, "Selection", edit.id, -1, 0, {}, true });
        store->metadataEdits.push_back (std::move (edit));
    }
    auto& draft = store->buildingPlans[building.key];
    std::ostringstream signature;
    for (const auto& floor : preview.plan.floors)
        signature << buildingplan::QuickSignature (floor, draft.cores, &draft.programme);
    for (const auto& [story, quick] : draft.quickPlans)
        signature << story << ':' << quick.signature << ':' << quick.revision;
    for (int story : draft.uniqueFloors)
        signature << 'u' << story;
    if (signature.str () != draft.previewFingerprint) {
        draft.previewFingerprint = signature.str ();
        changes.push_back ({ "massingPlanPreview", {}, "Selection", "local floor design", -1, 1, {}, true });
    }
    ImGui::PopID ();
}
} // namespace geomsrv::archviz::overlayhud
