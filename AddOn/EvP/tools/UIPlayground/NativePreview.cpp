#include "NativePreview.hpp"
#include "WorkflowState.hpp"
#include "ArchViz/GraphicsSettings.hpp"
#include "Geometry/MeshStore.hpp"
#include "Edit/EditPrimitives.hpp"
#include "Edit/EditPathEdit.hpp"
#include <imgui_internal.h>
#include <algorithm>
#include <cstring>

namespace tapioca::playground {
namespace shell = av::hudshell;
bool RestoreNative (NativePreview& p, const Value& state)
{
    const auto& saved = Member (state, "native");
    p.held = Text (saved, "held", "@massing");
    p.open = Flag (saved, "open", true);
    p.navigation.workflow = Text (saved, "workflow", "@massing");
    p.navigation.workflowTitle = Text (saved, "title", "Massing");
    p.dock.edge = shell::DockEdge (int (std::clamp (Number (saved, "dockEdge", 0), 0.0, 4.0)));
    p.dock.horizontal = Flag (saved, "dockHorizontal", true);
    p.dock.along = float (Number (saved, "dockAlong"));
    p.dock.freePosition = { float (Number (saved, "dockX", 100)), float (Number (saved, "dockY", 100)) };
    p.positioned = Flag (saved, "positioned");
    p.placement.placed = Flag (saved, "panelPlaced");
    p.placement.corner = uint8_t (std::clamp (Number (saved, "panelCorner"), 0.0, 3.0));
    p.placement.offset[0] = float (Number (saved, "panelX"));
    p.placement.offset[1] = float (Number (saved, "panelY"));
    p.mocks.hud.sunRunContext = { "fixture-building-a" };
    p.mocks.hud.visibilitySelection = { "fixture-building-a", "fixture-building-b" };
    p.mocks.hud.visibilityFrom = { "fixture-building-a" };
    p.mocks.hud.visibilityTo = { "fixture-building-b" };
    auto capture = std::make_shared<geomsrv::Snapshot> ();
    capture->id = 1;
    capture->captureStamp = 1;
    capture->scope = "all";
    capture->completeModel = true;
    for (int i = 0; i < 2; ++i) {
        geomsrv::Mesh mesh;
        mesh.guid = i == 0 ? "fixture-building-a" : "fixture-building-b";
        mesh.vertices = { double (i * 10), 0, 0, double (i * 10 + 6), 0, 0, double (i * 10), 8, 3 };
        mesh.triangles = { 0, 1, 2 };
        capture->meshes.push_back (std::move (mesh));
    }
    p.mocks.capture = capture;
    auto& sun = p.mocks.scene.sunStudy;
    sun.drawing = true;
    sun.studyId = "fixture-sun";
    sun.elementsAttached = 2;
    sun.stepCount = 9;
    sun.quantumHours = 1;
    sun.stepMinutes = { 480, 540, 600, 660, 720, 780, 840, 900, 960 };
    sun.noonStep = 4;
    p.mocks.hud.sunStepMinutes = sun.stepMinutes;
    if (!Member (saved, "values").Find ("editorDocument")) {
        geomsrv::edit::Document doc;
        geomsrv::edit::AddPolyline (doc, { { 0, 0, 0 }, { 6, 0, 0 }, { 6, 4, 0 }, { 0, 4, 0 } }, true,
                                    "fixture outline");
        const auto path =
            geomsrv::edit::AddPath (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 18, 6, 0.6 } }, 2, "fixture path");
        p.mocks.editor.edits = geomsrv::edit::Session (std::move (doc));
        p.mocks.editor.selection.Set ({ { path, geomsrv::edit::Part::Entity } });
    }
    return RestoreWorkflowState (p.mocks, Member (saved, "values"));
}
void StoreNative (NativePreview& p, Value& state)
{
    Value saved = Value::Object ({ { "held", Value::String (p.held) },
                                   { "open", Value::Bool (p.open) },
                                   { "workflow", Value::String (p.navigation.workflow) },
                                   { "title", Value::String (p.navigation.workflowTitle) },
                                   { "dockEdge", Value::Integer (int (p.dock.edge)) },
                                   { "dockHorizontal", Value::Bool (p.dock.horizontal) },
                                   { "dockAlong", Value::Double (p.dock.along) },
                                   { "dockX", Value::Double (p.dock.freePosition.x) },
                                   { "dockY", Value::Double (p.dock.freePosition.y) },
                                   { "positioned", Value::Bool (p.positioned) },
                                   { "panelPlaced", Value::Bool (p.placement.placed) },
                                   { "panelCorner", Value::Integer (p.placement.corner) },
                                   { "panelX", Value::Double (p.placement.offset[0]) },
                                   { "panelY", Value::Double (p.placement.offset[1]) } });
    Put (saved, "values", WorkflowState (p.mocks));
    Put (state, "native", std::move (saved));
}
void DrawNative (NativePreview& p, Value& state, ImVec2 stageMin, ImVec2 stageMax, ImVec2 view,
                 const av::overlaylayers::Panel& look, float dpi, float ui,
                 const std::function<void (const std::string&)>& fixturePage)
{
    p.mocks.document = &state;
    MockScope scope (p.mocks);
    const auto previous = WorkflowState (p.mocks);
    p.mocks.hud.width = uint32_t (view.x);
    p.mocks.hud.height = uint32_t (view.y);
    geomsrv::MeshStore::Get ().Publish (p.mocks.capture);
    if (!p.positioned) {
        p.dock.freePosition = { (stageMin.x + 8 * dpi) / dpi, (stageMin.y + 8 * dpi) / dpi };
        p.positioned = true;
    }
    std::vector<shell::HostTab> tabs {
        { shell::kStatsKey, "Stats" },
        { shell::kSelectionKey, "Selection" },
        { "@massing", "Massing", {}, av::ui::IconId::None, shell::WorkflowFamily::Design },
        { "@analysis", "Analysis", {}, av::ui::IconId::Glasses, shell::WorkflowFamily::Analyze },
        { "@editor", "Virtual editor", {}, av::ui::IconId::None, shell::WorkflowFamily::Design },
        { shell::kSettingsKey, "Settings" },
        { shell::kDebugKey, "Debug" }
    };
    const auto select = [&] (const std::string& key) {
        p.held = key;
        p.open = true;
        // The browser can explicitly request native Stats while the editor is active.
        if (p.navigation.workflow == "@editor" && (key == shell::kStatsKey || key == shell::kSelectionKey)) {
            p.navigation.workflow = "@massing";
            p.navigation.workflowTitle = "Massing";
        }
        for (const auto& tab : tabs)
            if (tab.key == key && key != shell::kStatsKey && key != shell::kSelectionKey &&
                key != shell::kSettingsKey && key != shell::kDebugKey) {
                p.navigation.workflow = key;
                p.navigation.workflowTitle = tab.title;
            }
        Event (state, "Native navigation: " + key);
    };
    const auto requested = Text (state, "nativeRequest");
    if (!requested.empty ()) {
        select (requested);
        Put (state, "nativeRequest", Value::String (""));
    }
    const auto visibleTabs = [&] {
        auto visible = tabs;
        // Match OverlayHudHost::WorkflowInfoTabs: native model Stats/Selection
        // do not belong to the virtual document; settings and debug still do.
        if (p.navigation.workflow == "@editor")
            std::erase_if (visible, [] (const auto& tab) {
                return tab.key == shell::kStatsKey || tab.key == shell::kSelectionKey;
            });
        return visible;
    };
    const shell::Circle ready { shell::Phase::Ready, true, -1, "Standalone UI fixture; no Archicad connection" };
    const auto dock = shell::Dock (p.dock, view, dpi, ui, nullptr, p.navigation.workflowTitle, look, p.open, ready,
                                   nullptr, nullptr, visibleTabs (), p.held);
    if (!dock.selected.empty ())
        select (dock.selected);
    if (dock.press.collapse)
        p.open = !p.open;
    if (p.open) {
        shell::HostSpec spec;
        spec.window = "###playground.native.hud";
        spec.look = &look;
        spec.scale = dpi;
        spec.ui = ui;
        spec.view = view;
        spec.attached = true;
        spec.dockMin = dock.window->Pos;
        spec.dockMax = dock.window->Pos + dock.window->Size;
        spec.dockEdge = p.dock.edge;
        spec.dockHorizontal = p.dock.horizontal;
        spec.tabs = visibleTabs ();
        spec.textInput = p.navigation.workflow != "@editor";
        const auto result = shell::Host (
            spec, p.held, p.shown, p.placement,
            [&] (const std::string& key) {
                ImGui::PushItemWidth (ImGui::GetContentRegionAvail ().x * 0.5f);
                if (key == "@editor")
                    av::edithud::Panel (p.mocks.editor);
                else if (key == "@analysis")
                    av::viewerhud::SunStudyPage (p.mocks.hud, p.mocks.scene);
                else if (key == shell::kSelectionKey) {
                    ImGui::TextUnformatted ("Mock selection");
                    for (const auto& guid : p.mocks.hud.visibilitySelection)
                        ImGui::TextUnformatted (guid.c_str ());
                    if (ImGui::Button ("Clear fixture selection"))
                        p.mocks.hud.visibilitySelection.clear ();
                    if (ImGui::Button ("Select both fixture buildings"))
                        p.mocks.hud.visibilitySelection = { "fixture-building-a", "fixture-building-b" };
                }
                else if (key == shell::kSettingsKey) {
                    ImGui::TextWrapped (
                        "Native 320 logical-pixel attached panel. DPI and text size scale it as in the add-on.");
                    bool reset = false;
                    shell::HudSettings (p.fontStep, p.placement, reset);
                    if (reset)
                        p.positioned = false;
                }
                else if (key == shell::kDebugKey)
                    ImGui::TextWrapped (
                        "UI-only fixtures. Runs, picks, imports and exports are recorded by the mock owner.");
                else
                    fixturePage (key);
                ImGui::PopItemWidth ();
            },
            nullptr, &p.navigation);
        if (!result.pressed.empty ())
            select (result.pressed);
        ImGui::BringWindowToDisplayFront (result.window);
    }
    if (p.mocks.editor.draftingImport) {
        p.mocks.editor.draftingImport = false;
        p.mocks.editor.message = "Mock import recorded; no Archicad selection was read.";
        Event (state, p.mocks.editor.message);
    }
    ImGui::BringWindowToDisplayFront (dock.window);
    for (const auto& popup : ImGui::GetCurrentContext ()->OpenPopupStack)
        if (popup.Window)
            ImGui::BringWindowToDisplayFront (popup.Window);
    if (p.mocks.editor.close) {
        p.mocks.editor.close = false;
        p.open = false;
        Event (state, "Virtual editor: Done (mock owner)");
    }
    if (p.mocks.hud.sunShotRequested) {
        p.mocks.hud.sunShotRequested = false;
        p.mocks.hud.sunShotResult = "Mock screenshot request recorded; no image was written.";
        p.mocks.hud.sunShotFailed = true;
        Event (state, p.mocks.hud.sunShotResult);
    }
    auto& h = p.mocks.hud;
    if (h.viewpointPlacing) {
        h.viewpointPlacing = false;
        p.mocks.viewpoint.options.point = { 4, 6, 1.6 };
        p.mocks.viewpoint.pointValid = true;
        Event (state, "Mock pick: viewpoint placed at fixture (4, 6, 1.6) m");
    }
    if (h.viewpointPlacingTarget) {
        h.viewpointPlacingTarget = false;
        h.visibilityTargetSet = h.visibilityTargetEdited = true;
        const double target[] = { 12, 4, 1.6 };
        std::copy (target, target + 3, h.visibilityTarget);
        Event (state, "Mock pick: target placed at fixture (12, 4, 1.6) m");
    }
    if (h.sunRunPickingCentre) {
        h.sunRunPickingCentre = false;
        h.sunRunCentre[0] = 8;
        h.sunRunCentre[1] = 4;
        Event (state, "Mock pick: sun cylinder centre set to fixture (8, 4) m");
    }
    if (h.viewpointPickingContext) {
        h.viewpointPickingContext = false;
        p.mocks.viewpoint.context = h.visibilitySelection;
        Event (state, "Mock pick: Context populated from fixture selection");
    }
    const auto after = WorkflowState (p.mocks);
    if (const auto* values = after.AsObject ())
        for (const auto& [key, value] : *values)
            if (json::Write (value, 0) != json::Write (Member (previous, key.c_str ()), 0))
                Event (state, p.navigation.workflowTitle + ": " + key + " changed (fixture)");
    StoreNative (p, state);
    (void) stageMax;
}
} // namespace tapioca::playground
