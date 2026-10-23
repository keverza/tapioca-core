// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.
// ArchViz/OverlayControllerHud -- the controller's HUD half (OverlayController.hpp): what the
// user did to the overlays' HUD, read and set for the verbs, and the renderers brought into
// line with it. Moved out of OverlayController.cpp whole when the HUD's own pages arrived;
// the controller's view-following and its sessions stay there.
//
// THREAD. Every entry point is MAIN THREAD.

#include "ArchViz/OverlayController.hpp"
#include "ArchViz/EditLayers.hpp"

#include "ArchViz/OverlayClickTiming.hpp"
#include "ArchViz/OverlayGuestText.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "ArchViz/OverlayInput.hpp"
#include "ArchViz/OverlayLayers.hpp"
#include "ArchViz/OverlayHudModel.hpp"
#include "ArchViz/OverlayVisibility.hpp"
#include "ArchViz/PlanOverlayRuntime.hpp"
#include "ArchViz/SelectionMetadata.hpp"
#include "ArchViz/MassingModel.hpp"
#include "ArchViz/MassingHybrid.hpp"
#include "ArchViz/MassingSlicesModel.hpp"
#include "ArchViz/MassingInspectionModel.hpp"
#include "ArchViz/SurfaceSwitch.hpp"
#include "ArchViz/TextPrompt.hpp"
#include "ArchViz/HudConsole.hpp"
#include "ArchViz/HudFloorPlanFiles.hpp"
#include "ArchViz/RoofSlopeBake.hpp"
#include "ArchViz/VirtualEditorTerrainCapture.hpp"
#include "Palette/ProgrammeDialog.hpp"

#include <algorithm>
#include <climits>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace overlaycontrol {

HudReport Hud ()
{
    HudReport out;
    const std::shared_ptr<overlayhud::State> state = guesttext::HudState ();
    out.fontScale = overlayhud::FontScaleOf (*state);
    out.open = overlayhud::HudOpen (*state);
    out.visible = overlayhud::ContentShown (*state);
    out.hover = overlayhud::HoverMode (*state);
    out.hiddenLayers = overlayhud::HiddenLayers (*state);
    const std::string selected = overlayhud::SelectedKey (*state);
    for (const auto& layer : overlaylayers::Layers ())
        for (size_t i = 0; i < layer->panels.size (); ++i) {
            const overlaylayers::Panel& panel = layer->panels[i];
            const std::string key = layer->name + "#" + std::to_string (i);
            HudPanel record { layer->name, uint32_t (i), panel.title, !panel.title.empty () && key == selected,
                              overlayhud::Values (*state, key) };
            if (!panel.title.empty () || !record.values.empty ())
                out.panels.push_back (std::move (record));
        }
    return out;
}

std::vector<std::shared_ptr<const overlaylayers::Layer>> ShownLayers ()
{
    std::vector<std::shared_ptr<const overlaylayers::Layer>> layers = overlaylayers::Layers ();
    const std::shared_ptr<overlayhud::State> state = guesttext::HudState ();
    layers.erase (std::remove_if (layers.begin (), layers.end (),
                                  [&] (const std::shared_ptr<const overlaylayers::Layer>& layer) {
                                      return !overlayhud::LayerShown (*state, layer->name);
                                  }),
                  layers.end ());
    return layers;
}

namespace {

// What the renderers last followed of the HUD's state.
uint64_t g_followedRevision = 0;
std::vector<std::string> g_followedHidden;

} // namespace

void FollowHudState ()
{
    massinghybrid::Poll ();
    massingslicesmodel::Poll ();
    const std::shared_ptr<overlayhud::State> state = guesttext::HudState ();
    bool recoloured = false;
    roofslopebake::Follow (state);
    FollowTerrainCapture (state);
    overlaylayers::Layer roof;
    bool clearRoof = false;
    if (overlayhud::FollowRoofEditor (*state, roof, clearRoof)) {
        if (clearRoof)
            overlaylayers::Clear (roofslope::kLayer);
        else
            overlaylayers::Set (std::move (roof));
        PublishLayers ();
        overlayinput::RequestLayout (overlayinput::View::ThreeD);
        overlayinput::RequestLayout (overlayinput::View::Plan);
    }
    // The virtual editor follows exactly as the roof does: its layer when the document or the
    // selection moved, and the same scoped keyboard ownership while it has the locked view.
    overlaylayers::Layer editorLayer;
    bool clearEditorLayer = false;
    if (overlayhud::FollowVirtualEditor (*state, editorLayer, clearEditorLayer)) {
        if (clearEditorLayer)
            overlaylayers::Clear (editlayers::kLayer);
        else
            overlaylayers::Set (std::move (editorLayer));
        PublishLayers ();
        overlayinput::RequestLayout (overlayinput::View::ThreeD);
        overlayinput::RequestLayout (overlayinput::View::Plan);
    }
    const bool roofKeys = overlayhud::RoofEditorActive (*state) && overlayhud::EditLocked (*state) &&
                          overlayhud::ContentShown (*state) && overlayhud::LayerShown (*state, roofslope::kLayer);
    const bool editorKeys = overlayhud::VirtualEditorActive (*state) && overlayhud::EditLocked (*state) &&
                            overlayhud::ContentShown (*state) && overlayhud::LayerShown (*state, editlayers::kLayer);
    overlayinput::EnableHudTextKeys (overlayhud::CriteriaTextActive (*state));
    if (!overlayinput::EnableRoofEditKeys (roofKeys || editorKeys)) {
        overlayhud::StopRoofEditor (*state);
        overlayhud::StopVirtualEditor (*state, "keyboard ownership was refused");
        overlaylayers::Clear (roofslope::kLayer);
        overlaylayers::Clear (editlayers::kLayer);
        PublishLayers ();
        overlayinput::RequestLayout (overlayinput::View::ThreeD);
        overlayinput::RequestLayout (overlayinput::View::Plan);
    }
    for (const auto& change : overlayhud::TakeRampCutoffs (*state)) {
        const std::string layer = change.key.substr (0, change.key.rfind ('#'));
        recoloured = overlaylayers::SetRampCutoff (layer, change.id, change.value) || recoloured;
    }
    for (const auto& change : overlayhud::TakeRampGreens (*state)) {
        const std::string layer = change.key.substr (0, change.key.rfind ('#'));
        recoloured = overlaylayers::SetRampGreen (layer, change.id, change.value) || recoloured;
    }
    if (recoloured) {
        PublishLayers ();
        overlayinput::RequestLayout (overlayinput::View::ThreeD);
        overlayinput::RequestLayout (overlayinput::View::Plan);
    }
    // The dock's viewer circle pressed: the switch, from the message loop's top -- not inside the
    // layout that took the press (SurfaceSwitch.hpp).
    if (overlayhud::TakeViewerRequest (*state))
        surfaceswitch::Request (surfaceswitch::Surface::Viewer);
    if (overlayhud::TakeLogsRequest (*state))
        hudconsole::OpenLogs ();
    overlaylayers::Layer stairs, units, planUnits;
    // The floor plan shows its own storey's units (user, 2026-10-14); its storey is the plan
    // session's, read here on the main thread like the rest of this turn.
    overlayhud::SetPlanStorey (*state, planruntime::Running () ? planruntime::GetStatus ().storey : INT_MIN);
    if (overlayhud::TakeFloorPlanLayers (*state, stairs, units, &planUnits)) {
        overlaylayers::Set (std::move (stairs));
        overlaylayers::Set (std::move (units));
        overlaylayers::Set (std::move (planUnits));
        PublishLayers ();
        overlayinput::RequestLayout (overlayinput::View::ThreeD);
        overlayinput::RequestLayout (overlayinput::View::Plan);
    }
    for (const auto& request : overlayhud::TakeMassingRequests (*state))
        massingmodel::Request (request);
    for (auto& edit : overlayhud::TakeMassingRuleEdits (*state))
        massingmodel::RequestRules (std::move (edit));
    for (auto& request : overlayhud::TakeMassingCalculations (*state))
        massinghybrid::Request (std::move (request));
    massinghybrid::Dimensions (overlayhud::MassingDimensions (*state));
    massingslicesmodel::HoverFunction (overlayhud::MassingStatsFunction (*state));
    massingslicesmodel::CollapseZone (overlayhud::MassingCollapseZone (*state));
    massinginspectionmodel::Follow (overlayhud::UniqueBuildings (*state), overlayhud::HighlightedBuilding (*state),
                                    overlayhud::BuildingFloorKey (*state), overlayhud::BuildingFloors (*state),
                                    overlayhud::MarkLargeFloors (*state), overlayhud::MassingCoefficients (*state),
                                    overlayhud::ShowLowHeadroom (*state));
    for (const auto& edit : overlayhud::TakeMassingCoefficientNumbers (*state)) {
        std::string answer;
        double number = 0;
        if (textprompt::Ask (edit.key + " (" + hudmeta::NumberText (edit.min) + " - " + hudmeta::NumberText (edit.max) +
                                 ")",
                             hudmeta::NumberText (edit.number), answer)) {
            if (!hudmeta::ParseNumber (answer, edit.min, edit.max, number))
                hudconsole::Warning ("Area coefficients", "Enter a finite number within the displayed bounds.");
            else if (!overlayhud::AnswerMassingCoefficientNumber (*state, edit, number))
                hudconsole::Warning ("Area coefficients",
                                     "Settings changed while the prompt was open; value not applied.");
        }
        overlayinput::RequestLayout (overlayinput::View::ThreeD);
        overlayinput::RequestLayout (overlayinput::View::Plan);
    }
    for (const auto& edit : overlayhud::TakeMassingNumbers (*state)) {
        std::string answer;
        double number = 0;
        if (textprompt::Ask (edit.key + " (" + hudmeta::NumberText (edit.min) + " - " + hudmeta::NumberText (edit.max) +
                                 ")",
                             hudmeta::NumberText (edit.number), answer)) {
            if (!hudmeta::ParseNumber (answer, edit.min, edit.max, number))
                hudconsole::Warning ("Massing input", "Enter a finite number within the displayed bounds.");
            else if (!overlayhud::AnswerMassingNumber (*state, edit, number))
                hudconsole::Warning ("Massing input", "Inputs changed while the panel was open; value not applied.");
        }
        overlayinput::RequestLayout (overlayinput::View::ThreeD);
        overlayinput::RequestLayout (overlayinput::View::Plan);
    }
    for (auto& request : overlayhud::TakeMassingCalculations (*state))
        massinghybrid::Request (std::move (request));
    // Define Programme's typed rows: the same native prompt as the numeric Set panels.
    for (const auto& edit : overlayhud::TakeProgrammeTexts (*state)) {
        std::string answer, error;
        if (textprompt::Ask (edit.type < 0 ? "Programme: share% rooms room min-max m2, types separated by ;"
                                           : "Flat type: share% rooms room min-max m2",
                             edit.text, answer) &&
            !overlayhud::AnswerProgrammeText (*state, edit, answer, error))
            hudconsole::Warning ("Programme", error);
        overlayinput::RequestLayout (overlayinput::View::ThreeD);
        overlayinput::RequestLayout (overlayinput::View::Plan);
    }
    for (const auto& edit : overlayhud::TakeProgrammeTables (*state)) {
        const auto epoch = massingmodel::ProjectEpoch ();
        const auto saved = massingmodel::Read ();
        floorprogramme::Programme answer;
        const bool accepted = evp::programmeui::Ask (edit, answer);
        // A modal pumps the host loop, even on Cancel. Never drain the old HUD's remaining
        // saves or actions into a different project after it returns.
        if (epoch != massingmodel::ProjectEpoch () || state != guesttext::HudState ()) {
            hudconsole::Warning ("Programme", "The project changed while the table was open; not applied.");
            return;
        }
        if (accepted) {
            const auto current = massingmodel::Read ();
            std::string error;
            if (!current.known || !(saved.programme == current.programme))
                hudconsole::Warning ("Programme", "The project or saved programme changed; table not applied.");
            else if (!overlayhud::AnswerProgrammeTable (*state, edit, answer, error))
                hudconsole::Warning ("Programme", error);
        }
        overlayinput::RequestLayout (overlayinput::View::ThreeD);
        overlayinput::RequestLayout (overlayinput::View::Plan);
    }
    for (const auto& edit : overlayhud::TakeProgrammeColours (*state)) {
        const auto epoch = massingmodel::ProjectEpoch ();
        const auto saved = massingmodel::Read ();
        uint32_t rgba = 0;
        const bool accepted = evp::programmeui::AskColour (edit, rgba);
        if (epoch != massingmodel::ProjectEpoch () || state != guesttext::HudState ()) {
            hudconsole::Warning ("Programme", "The project changed while the colour picker was open; not applied.");
            return;
        }
        if (accepted) {
            const auto current = massingmodel::Read ();
            std::string error;
            if (!current.known || !(saved.programme == current.programme))
                hudconsole::Warning ("Programme", "The project or saved programme changed; colour not applied.");
            else if (!overlayhud::AnswerProgrammeColour (*state, edit, rgba, error))
                hudconsole::Warning ("Programme", error);
        }
        overlayinput::RequestLayout (overlayinput::View::ThreeD);
        overlayinput::RequestLayout (overlayinput::View::Plan);
    }
    // A finished programme edit is stored in the project; Plan view exports become files.
    floorprogramme::Programme programme;
    if (overlayhud::TakeProgrammeSave (*state, programme))
        massingmodel::RequestProgramme (std::move (programme));
    for (const auto& file : overlayhud::TakePlanExports (*state))
        planfiles::Write (file);
    // The Selection page's metadata edits: written from the message loop too, in one undo step,
    // to every element selected then; the page is read again after (SelectionMetadata.hpp).
    std::vector<hudmeta::Edit> edits = overlayhud::TakeMetadataEdits (*state);
    std::vector<hudmeta::Edit> targeted, selected;
    std::map<std::string, std::vector<hudmeta::Edit>> buildingEdits;
    std::vector<std::string> targets;
    for (auto& edit : edits)
        if (!edit.expectedBuildingKey.empty ())
            buildingEdits[edit.expectedBuildingKey].push_back (std::move (edit));
        else if (edit.element.empty ())
            selected.push_back (std::move (edit));
        else {
            targets.push_back (edit.element);
            targeted.push_back (std::move (edit));
        }
    if (!selected.empty ())
        selectionmetadata::Request (std::move (selected), selectionmetadata::SelectedGuids (),
                                    [] () { overlayhudmodel::SelectionChanged (); });
    if (!targeted.empty ()) {
        std::sort (targets.begin (), targets.end ());
        targets.erase (std::unique (targets.begin (), targets.end ()), targets.end ());
        selectionmetadata::Request (std::move (targeted), std::move (targets),
                                    [] () { overlayhudmodel::SelectionChanged (); });
    }
    for (auto& [building, assigned] : buildingEdits) {
        std::vector<std::string> members;
        for (const auto& edit : assigned)
            members.push_back (edit.element);
        selectionmetadata::Request (std::move (assigned), std::move (members),
                                    [] () { overlayhudmodel::SelectionChanged (); });
    }
    // Automatic function-coloured massing slices replace the legacy accent-only highlight.
    // Settings' displays, as the user set them: switched, or a new look on what was cut.
    overlayhud::Displays displays;
    if (overlayhud::TakeDisplays (*state, displays))
        overlayhudmodel::ApplyDisplays (displays);
    for (const auto kind : overlayhud::TakeMassingBakes (*state))
        massingbake::Request (kind);
    const uint64_t revision = overlayhud::Revision (*state);
    if (revision == g_followedRevision)
        return;
    g_followedRevision = revision;
    // Shown or hidden as a whole: read at Present, nothing rebuilt.
    overlayvisibility::SetContentShown (overlayhud::ContentShown (*state));
    // Hover mode: read by the input hook, which then follows the pointer over the whole view.
    overlayvisibility::SetHovering (overlayhud::HoverMode (*state));
    // A layer hidden or shown again: the content is rebuilt without it -- the 3D view's now,
    // the plan's at its next tick, as the store moved.
    std::vector<std::string> hidden = overlayhud::HiddenLayers (*state);
    if (hidden != g_followedHidden) {
        g_followedHidden = std::move (hidden);
        overlaylayers::Touch ();
        PublishLayers ();
    }
    // Both views' HUDs follow -- the dock's circle, the tabs of a hidden layer -- and draw.
    overlayinput::RequestLayout (overlayinput::View::ThreeD);
    overlayinput::RequestLayout (overlayinput::View::Plan);
}

namespace {
std::string g_runningCommand;
}

void ClearCommandPreviews (const std::string& next)
{
    g_runningCommand = next;
    overlayhud::State& state = *guesttext::HudState ();
    const bool keepEditor = !next.empty () && overlayhud::VirtualEditorOwnedBy (state, next);
    overlayhud::StopRoofEditor (state);
    if (!keepEditor) {
        overlayhud::StopVirtualEditor (state, next.empty () ? "a project boundary" : "another command ran");
        overlayinput::EnableRoofEditKeys (false);
    }
    if (overlaylayers::ClearCommandScoped (keepEditor ? editlayers::kLayer : ""))
        PublishLayers ();
}

const std::string& RunningCommand ()
{
    return g_runningCommand;
}

void SetOverlayVisible (bool visible)
{
    overlayhud::SetContentShown (*guesttext::HudState (), visible);
    FollowHudState ();
}

void SetLayerVisible (const std::string& layer, bool visible)
{
    overlayhud::SetLayerShown (*guesttext::HudState (), layer, visible);
    FollowHudState ();
}

void SetHudOpen (bool open)
{
    overlayhud::SetHudOpen (*guesttext::HudState (), open);
    overlayinput::RequestLayout (overlayinput::View::ThreeD);
    overlayinput::RequestLayout (overlayinput::View::Plan);
}

bool SelectHudPanel (const std::string& layer, uint32_t panel)
{
    for (const auto& set : overlaylayers::Layers ())
        if (set->name == layer && panel < set->panels.size () && !set->panels[panel].title.empty ()) {
            overlayhud::SelectKey (*guesttext::HudState (), layer + "#" + std::to_string (panel));
            overlayinput::RequestLayout (overlayinput::View::ThreeD);
            overlayinput::RequestLayout (overlayinput::View::Plan);
            return true;
        }
    return false;
}

void TimeHudClicks (bool on)
{
    overlayinput::TimeClicks (on);
}

ClickReport HudClicks ()
{
    ClickReport out;
    out.armed = overlayinput::TimingClicks ();
    out.idles = overlayinput::ClickIdles ();
    const std::vector<overlayclicks::Sample> samples = overlayinput::ClickSamples ();
    uint64_t newest = 0;
    for (const overlayclicks::Sample& sample : samples)
        newest = (std::max) (newest, sample.at);
    for (const overlayclicks::Sample& sample : samples) {
        ClickRecord record;
        record.target = overlayclicks::TargetName (sample.target);
        record.windowClass = sample.windowClass;
        record.complete = sample.complete;
        record.ageMilliseconds = uint32_t ((newest - sample.at) / 1000u);
        record.busyMicroseconds = sample.busyMicroseconds;
        record.firstIdleMicroseconds = sample.firstIdleMicroseconds;
        record.bursts = sample.bursts;
        record.layoutMicroseconds = sample.layoutMicroseconds;
        record.layouts = sample.layouts;
        record.redrawMicroseconds = sample.redrawMicroseconds;
        record.redraws = sample.redraws;
        out.clicks.push_back (std::move (record));
    }
    return out;
}

void SetHoverMode (bool on)
{
    // The readout is in the HUD's floating panel: on, the panel opens to show it.
    if (on)
        overlayhud::SetHudOpen (*guesttext::HudState (), true);
    overlayhud::SetHoverMode (*guesttext::HudState (), on);
    FollowHudState ();
}

void SetHudFontScale (float scale)
{
    overlayhud::SetFontScale (*guesttext::HudState (), scale);
    overlayinput::RequestLayout (overlayinput::View::ThreeD);
    overlayinput::RequestLayout (overlayinput::View::Plan);
}

} // namespace overlaycontrol
} // namespace archviz
} // namespace geomsrv
