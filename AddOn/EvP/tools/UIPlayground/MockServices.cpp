#include "NativePreview.hpp"
#include "ArchViz/HudVirtualEditorDebug.hpp"
#include "ArchViz/VisibilityStudyCapture.hpp"
#include "ArchViz/ViewpointBake.hpp"
#include "ArchViz/AnalysisPathSource.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "SunStudy/SunStudyLimits.hpp"
#include "Geometry/MeshStore.hpp"
#include <stdexcept>

namespace tapioca::playground {
namespace {
MockServices* s_current = nullptr;
}
MockServices& Services ()
{
    if (!s_current)
        throw std::logic_error ("Mock service called outside playground Draw");
    return *s_current;
}
MockScope::MockScope (MockServices& services) : previous (s_current)
{
    s_current = &services;
}
MockScope::~MockScope ()
{
    s_current = previous;
}
void LogMock (const std::string& text)
{
    Event (*Services ().document, "Mock owner: " + text);
}
} // namespace tapioca::playground
namespace pg = tapioca::playground;
namespace geomsrv::sunstudyhud {
bool Submit (const Request&, std::string& error)
{
    auto& m = pg::Services ();
    error.clear ();
    m.sun.stage = "done";
    m.sun.studyId = "fixture-sun";
    ++m.sun.runs;
    m.scene.sunStudy.drawing = true;
    m.scene.sunStudy.analysisKind = 0;
    m.scene.sunStudy.studyId = "fixture-sun";
    pg::LogMock ("Run sun study -> deterministic fixture; no analysis worker");
    return true;
}
void Cancel ()
{
    pg::Services ().sun.running = false;
    pg::LogMock ("Cancel sun study");
}
Status Current ()
{
    return pg::Services ().sun;
}
} // namespace geomsrv::sunstudyhud
namespace geomsrv::archviz {
void ArchVizLog (const std::string& message)
{
    pg::LogMock (message);
}
namespace edithud {
bool RequestDebugExport (Session& s)
{
    s.debugExportRequested = false;
    s.message = "Mock export recorded; no file written.";
    pg::LogMock (s.message);
    return true;
}
} // namespace edithud
namespace viewpointstudy {
void Configure (Settings settings)
{
    settings.revision = pg::Services ().viewpoint.revision + 1;
    pg::Services ().viewpoint = std::move (settings);
}
std::shared_ptr<const Settings> GetSettings ()
{
    return std::make_shared<const Settings> (pg::Services ().viewpoint);
}
Status GetStatus ()
{
    return pg::Services ().viewpointStatus;
}
} // namespace viewpointstudy
namespace visibilitystudy {
bool Submit (Request request, std::string& error)
{
    auto& m = pg::Services ();
    error.clear ();
    m.visibility.complete = true;
    m.visibility.running = false;
    m.visibility.studyId = "fixture-visibility";
    m.visibility.origin = int (request.options.origin);
    m.visibility.meanVisibility = 0.64;
    m.visibility.sampleCount = 256;
    m.scene.sunStudy.drawing = true;
    m.scene.sunStudy.analysisKind = 1;
    m.scene.sunStudy.studyId = m.visibility.studyId;
    m.hud.analysisFocus.shownOrigin = m.visibility.origin;
    pg::LogMock ("Run visibility -> deterministic 64% fixture; no rays traced");
    return true;
}
Status GetStatus ()
{
    return pg::Services ().visibility;
}
void Cancel ()
{
    pg::Services ().visibility.running = false;
    pg::LogMock ("Cancel visibility");
}
void ClearDisplay ()
{
    pg::Services ().scene.sunStudy.drawing = false;
    pg::Services ().visibility = {};
    pg::LogMock ("Clear visibility");
}
} // namespace visibilitystudy
namespace analysispathsource {
void RequestImport ()
{
    auto& h = pg::Services ().hud;
    h.analysisPath = std::make_shared<analysispath::Path> (
        analysispath::Build ("fixture-path", { 0, 0, 12, 0, 12, 8 }, {}, false, 0));
    h.analysisProfile = analysisprofile::Flat (h.analysisPath->Length (), 1.6);
    pg::LogMock ("Import analysis path -> deterministic fixture; no Archicad read");
}
void RequestSave (std::string, analysisprofile::Profile)
{
    pg::Services ().hud.analysisProfileDirty = false;
    pg::Services ().hud.analysisProfileNote = "Saved in mock owner only.";
    pg::LogMock ("Save analysis profile (mock; no project write)");
}
void RequestRefresh (std::string, uint64_t)
{
}
Imported Latest ()
{
    return {};
}
Saved LastSave ()
{
    return {};
}
} // namespace analysispathsource
namespace viewpointbake {
void RequestBake (evp::sunstudy::ViewpointPlanContour)
{
    pg::LogMock ("Bake viewpoint (mock; no Fill created)");
}
Baked LastBake ()
{
    return {};
}
} // namespace viewpointbake
} // namespace geomsrv::archviz
