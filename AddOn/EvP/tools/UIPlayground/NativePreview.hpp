#pragma once
#include "Fixture.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/DiligentHudShell.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/ViewpointStudyController.hpp"
#include "ArchViz/VisibilityStudyController.hpp"
#include "NativeCommands/SunStudyHudRunner.hpp"

namespace tapioca::playground {
struct MockServices {
    Value* document = nullptr;
    av::HudState hud;
    av::DiligentSceneStats scene;
    av::edithud::Session editor;
    av::viewpointstudy::Settings viewpoint;
    av::viewpointstudy::Status viewpointStatus;
    av::visibilitystudy::Status visibility;
    geomsrv::sunstudyhud::Status sun;
    std::shared_ptr<const geomsrv::Snapshot> capture;
};
// Scoped to a Draw call. No Archicad controllers or analysis workers are linked.
MockServices& Services ();
struct MockScope {
    explicit MockScope (MockServices& services);
    ~MockScope ();
    MockServices* previous;
};
struct NativePreview {
    MockServices mocks;
    av::hudshell::DockPlacement dock;
    av::hudshell::Placement placement;
    av::hudshell::NavigationState navigation;
    std::string held = "@massing", shown;
    bool open = true, positioned = false;
    uint32_t fontStep = av::hudshell::kFontStepDefault;
};
bool RestoreNative (NativePreview& preview, const Value& state);
void StoreNative (NativePreview& preview, Value& state);
void DrawNative (NativePreview& preview, Value& state, ImVec2 stageMin, ImVec2 stageMax, ImVec2 view,
                 const av::overlaylayers::Panel& look, float dpi, float ui,
                 const std::function<void (const std::string&)>& fixturePage);
} // namespace tapioca::playground
