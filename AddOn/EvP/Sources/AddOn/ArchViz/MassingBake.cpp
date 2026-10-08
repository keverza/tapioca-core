#include "APIEnvir.h"
#include "ACAPinc.h"
#include "ArchViz/MassingBake.hpp"
#include "Palette/MassingBakeDialog.hpp"
#include "ArchViz/MassingSlicesModel.hpp"
#include "ArchViz/MassingHybrid.hpp"
#include "ArchViz/SelectionMetadata.hpp"
#include "ArchViz/HudConsole.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "Python/PythonHost.hpp"
#include "Python/MainThreadGate.hpp"
#include "Python/PathUtils.hpp"
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <thread>

namespace geomsrv::archviz::massingbake {
void RejectedSlab (const std::vector<SliceChain>& rings, const std::string& group, double z, uint64_t token,
                   size_t sourceIndex)
{
    namespace js = evp::nodegraph::json;
    using V = js::JsonValue;
    js::JsonArray contours;
    double minEdge = 1e300;
    for (const auto& ring : rings) {
        js::JsonArray points;
        for (size_t i = 0; i < ring.Count (); ++i) {
            const size_t j = (i + 1) % ring.Count ();
            minEdge = (std::min) (minEdge, std::hypot (ring.xy[i * 2] - ring.xy[j * 2],
                                                       ring.xy[i * 2 + 1] - ring.xy[j * 2 + 1]));
            points.push_back (
                V::Object ({ { "x", V::Double (ring.xy[i * 2]) }, { "y", V::Double (ring.xy[i * 2 + 1]) } }));
        }
        contours.push_back (V::Array (std::move (points)));
    }
    const auto fixture = V::Object ({ { "format", V::String ("tapioca.massing-bake.rejected") },
                                      { "version", V::Integer (1) },
                                      { "units", V::String ("m") },
                                      { "group", V::String (group) },
                                      { "z", V::Double (z) },
                                      { "minimumEdge", V::Double (minEdge) },
                                      { "contours", V::Array (std::move (contours)) } });
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::system_clock::now ().time_since_epoch ())
            .count ();
    const auto root = evp::EvpDataDir ();
    if (root.IsEmpty ())
        return;
    const GS::UniString directory (root + GS::UniString ("\\logs"));
    const std::string name = "\\massing-bake-rejected-" + std::to_string (milliseconds) + "-" + std::to_string (token) +
                             "-" + std::to_string (sourceIndex) + ".json";
    const GS::UniString path (directory + GS::UniString (name.c_str (), CC_UTF8));
    GS::UniString error;
    if (evp::CreateDirectoryChain (directory) && evp::WriteTextFile (path, js::Write (fixture, 2).c_str (), error))
        ArchVizLog ("MASSING BAKE rejected polygon fixture: " +
                    std::string (path.ToCStr (0, MaxUSize, CC_UTF8).Get ()));
}
namespace {
namespace js = evp::nodegraph::json;
std::thread s_worker;
std::atomic<bool> s_running { false };
uint64_t s_nextToken = 0, s_activeToken = 0;
uint64_t s_epoch = 0;
bool s_dialog = false;
void Run (Kind kind)
{
    if (s_running.load () || s_dialog) {
        hudconsole::Warning ("Massing bake", "A bake is already open/running. Nothing queued for duplicate creation.");
        return;
    }
    massinghybrid::Poll ();
    massingslicesmodel::Changed ();
    massingslicesmodel::Poll ();
    const auto zone = kind == Kind::Collapse ? massingslicesmodel::CollapseContours () : nullptr;
    const auto slices = massingslicesmodel::Read ();
    const auto page = massinghybrid::Read ();
    js::JsonValue geometry, settings;
    std::string error;
    if (page.busy || !Geometry (kind, slices.get (), page.preview ? &page.preview->result : nullptr,
                                zone ? *zone : std::vector<SliceChain> {}, geometry, error)) {
        hudconsole::Warning ("Massing bake", page.busy ? "Wait for the current envelope calculation." : error);
        return;
    }
    s_dialog = true;
    const uint64_t token = ++s_nextToken;
    s_activeToken = token;
    const auto action = evp::massingbakeui::AskSettings (kind, settings);
    s_dialog = false;
    if (action == evp::massingbakeui::Action::Cancel || !Current (token))
        return;
    // A DG dialog has its own loop: validate the adopted snapshots after it closes.
    massinghybrid::Poll ();
    massingslicesmodel::Poll ();
    if (slices != massingslicesmodel::Read () || page.preview != massinghybrid::Read ().preview ||
        (kind == Kind::Collapse && zone != massingslicesmodel::CollapseContours ()) || massinghybrid::Read ().busy) {
        hudconsole::Warning ("Massing bake",
                             "Preview changed while settings were open. Reopen Bake for current geometry.");
        return;
    }
    if (action == evp::massingbakeui::Action::Export2D) {
        GS::UniString path;
        s_dialog = true;
        const bool chosen = evp::massingbakeui::AskExportPath (path);
        s_dialog = false;
        if (!chosen || !Current (token))
            return;
        // Save also runs a native modal loop. Do not export a stale fixture if
        // the project or its current operated preview changed while choosing a file.
        massinghybrid::Poll ();
        massingslicesmodel::Poll ();
        if (slices != massingslicesmodel::Read () || page.preview != massinghybrid::Read ().preview ||
            massinghybrid::Read ().busy) {
            hudconsole::Warning ("Massing export", "Preview changed while choosing a file. Reopen Export 2D.");
            return;
        }
        std::string text;
        GS::UniString writeError;
        if (!slices || !Export2DJson (*slices, text, error))
            hudconsole::Error ("Massing export", error);
        else if (!evp::WriteTextFile (path, text.c_str (), writeError))
            hudconsole::Error ("Massing export", writeError.ToCStr (0, MaxUSize, CC_UTF8).Get ());
        else {
            const std::string saved = path.ToCStr (0, MaxUSize, CC_UTF8).Get ();
            ArchVizLog ("MASSING EXPORT 2D " + saved);
            hudconsole::Note ("Massing export", "Saved story-slice 2D contours (JSON): " + saved);
        }
        return; // Export never initializes Python or dispatches a project write.
    }
    GS::UniString initializeError;
    if (!evp::PythonHost::Get ().EnsureInitialized (initializeError)) {
        hudconsole::Error ("Massing bake", initializeError.ToCStr (0, MaxUSize, CC_UTF8).Get ());
        return;
    }
    const auto input = Inputs (geometry, settings, token);
    if (s_worker.joinable ())
        s_worker.join (); // previous worker already finished
    s_running.store (true);
    hudconsole::Say (hudconsole::Level::Note, "Massing bake",
                     kind == Kind::Envelope ? "Creating model copies; Morph creation/finalization use two Undo steps."
                                            : "Creating non-source copies in one native Undo step.");
    s_worker = std::thread ([input, token] () {
        GS::UniString result, bridgeError;
        bool called = false;
        try {
            called = evp::PythonHost::Get ().RunGraphScript (
                "from tapioca.massing.bake import execute\npayload = execute(request)\n", "<native Massing bake>",
                GS::UniString (input.c_str (), CC_UTF8), "[{\"portId\":\"payload\"}]", "[]", 120000, result,
                bridgeError);
        }
        catch (const std::exception& exception) {
            bridgeError = GS::UniString (exception.what (), CC_UTF8);
        }
        const std::string message = (called ? result : bridgeError).ToCStr (0, MaxUSize, CC_UTF8).Get ();
        ArchVizLog ("MASSING BAKE LEDGER " + message);
        std::string note = called ? "Malformed bake ledger; inspect the project before retrying." : message;
        bool complete = false;
        const auto parsed = js::Parse (message);
        if (called && parsed.ok) {
            const auto* outputs = parsed.value.Find ("outputs");
            const auto* payload = outputs ? outputs->Find ("payload") : nullptr;
            const auto* text = payload ? payload->Find ("note") : nullptr;
            const auto* state = payload ? payload->Find ("state") : nullptr;
            std::string status;
            if (text && state && text->AsString (note) && state->AsString (status))
                complete = status == "complete";
        }
        selectionmetadata::Later ([complete, note, token] () {
            if (!Current (token))
                return;
            hudconsole::Say (complete ? hudconsole::Level::Note : hudconsole::Level::Error, "Massing bake",
                             note + " Created GUID ledger: archviz.log.");
            massingslicesmodel::Changed ();
        });
        s_running.store (false);
    });
}
} // namespace
void Request (Kind kind)
{
    GS::UniString error;
    const uint64_t epoch = s_epoch;
    // Post, never Invoke: DG modal user time must not hold a timed gate rendezvous.
    if (!evp::MainThreadGate::Get ().Post (
            [kind, epoch] () {
                if (epoch == s_epoch)
                    Run (kind);
            },
            error))
        hudconsole::Error ("Massing bake", error.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}
bool Current (uint64_t token)
{
    return token != 0 && token == s_activeToken;
}
void Forget ()
{
    s_activeToken = 0; // Worker checks the main-thread guard before each API stage.
    ++s_epoch;
}
void Shutdown ()
{
    Forget ();
    if (s_worker.joinable ())
        s_worker.join ();
}
} // namespace geomsrv::archviz::massingbake
