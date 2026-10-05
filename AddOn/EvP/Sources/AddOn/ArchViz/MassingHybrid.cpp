#include "APIEnvir.h"
#include "ACAPinc.h"
#include "Model.hpp"

#include "ArchViz/MassingHybrid.hpp"
#include "ArchViz/MassingModel.hpp"
#include "ArchViz/MassingRulesModel.hpp"
#include "ArchViz/SelectionMetadata.hpp"
#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/HudConsole.hpp"
#include "ArchViz/OverlayInput.hpp"
#include "ArchViz/OverlayController.hpp"
#include "Geometry/GeometryExtractor.hpp"
#include "Python/MainThreadGate.hpp"
#include "Python/PythonHost.hpp"

#include <atomic>
#include <algorithm>
#include <mutex>
#include <optional>
#include <thread>

namespace geomsrv::archviz::massinghybrid {
namespace {
namespace calc = massingcalculation;
constexpr char kLayer[] = "tapioca.massing.envelope";
constexpr char kSite[] = "tapioca.massing.lines";
struct Source {
    calc::Request request;
    std::string terrain;
    std::vector<std::string> terrainSet;
    std::vector<uint64_t> parcelStamps;
    uint64_t terrainStamp = 0, generation = 0;
};
struct Completion {
    Source source;
    calc::Preview preview;
    std::string error;
};
Page s_page;
std::optional<Source> s_source;
calc::PreviewQueue s_queue;
UINT_PTR s_timer = 0;
std::string s_observed;
bool s_polling = false;
bool s_dimensions = false;
std::shared_ptr<const geomsrv::Mesh> s_terrain;
uint64_t s_terrainStamp = 0;
uint64_t s_terrainRetryAt = 0;
unsigned s_terrainRetries = 0;
std::thread s_worker;
std::atomic<bool> s_running { false };
std::atomic<uint64_t> s_revision { 0 };
std::mutex s_mutex;
std::optional<Completion> s_completed;

uint64_t Stamp (const std::string& guid)
{
    API_Elem_Head head {};
    head.guid = APIGuidFromString (guid.c_str ());
    return ACAPI_Element_GetHeader (&head) == NoError ? uint64_t (head.modiStamp) : 0;
}

bool Current (const Source& source)
{
    massingmodel::Changed ();
    const auto current = massingmodel::Read ();
    const size_t terrainGroup = source.request.landscape == 0 ? 1 : 2;
    const auto parcels = calc::Expand (source.request);
    if (!current.known || current.parcels.size () != parcels.size () ||
        source.parcelStamps.size () != parcels.size () || current.guids[terrainGroup] != source.terrainSet ||
        (!source.terrain.empty () && Stamp (source.terrain) != source.terrainStamp))
        return false;
    for (size_t i = 0; i < parcels.size (); ++i) {
        const auto& before = parcels[i].before;
        const auto& now = current.parcels[i];
        if (!now.known || now.guid != before.guid || !massingrules::SameGeometry (now.edges, before.edges) ||
            now.hasStored != before.hasStored || now.stored != before.stored ||
            Stamp (before.guid) != source.parcelStamps[i])
            return false;
    }
    return true;
}

void Publish ()
{
    overlaycontrol::PublishLayers (); // Set alone does not refresh the retained 3D streams.
    overlayinput::RequestLayout (overlayinput::View::ThreeD);
    overlayinput::RequestLayout (overlayinput::View::Plan);
}

void ClearPublished ()
{
    s_revision.store (s_queue.Revision ());
    const bool envelope = overlaylayers::Clear (kLayer);
    const bool site = overlaylayers::Clear (kSite);
    const bool dimensions = overlaylayers::Clear (calc::kDimensions);
    s_source.reset ();
    s_page = {};
    if (envelope || site || dimensions)
        Publish ();
}

void Refuse (std::string error)
{
    s_page.note = std::move (error);
    ArchVizLog ("MASSING PYTHON  refused: " + s_page.note);
    hudconsole::Say (hudconsole::Level::Error, "Massing calculation", s_page.note);
}

void Start (Source source)
{
    if (source.generation != s_queue.Revision ())
        return;
    const auto parcels = calc::Expand (source.request);
    for (const auto& parcel : parcels)
        source.parcelStamps.push_back (Stamp (parcel.before.guid));
    source.terrainStamp = source.terrain.empty () ? 0 : Stamp (source.terrain);
    if (std::find (source.parcelStamps.begin (), source.parcelStamps.end (), uint64_t (0)) !=
            source.parcelStamps.end () ||
        !Current (source)) {
        Refuse ("Defined sources changed before preview; waiting for current inputs.");
        return;
    }
    GS::UniString initializeError;
    if (!evp::PythonHost::Get ().EnsureInitialized (initializeError)) {
        Refuse (initializeError.ToCStr (0, MaxUSize, CC_UTF8).Get ());
        return;
    }
    geomsrv::Mesh terrain;
    if (!source.terrain.empty ()) {
        if (!s_terrain || s_terrain->guid != source.terrain || s_terrainStamp != source.terrainStamp) {
            s_terrain.reset ();
            API_Element element {};
            element.header.guid = APIGuidFromString (source.terrain.c_str ());
            API_ElemInfo3D info {};
            ModelerAPI::Model model;
            // Convert the defined Mesh on demand after model edits, rather than
            // assigning a fresh model stamp to an old 3D-window snapshot.
            if (ACAPI_Element_Get (&element) == NoError &&
                ACAPI_ModelAccess_Get3DInfo (element.header, &info) == NoError && AcquireCurrentModel (model))
                for (int32_t i = 1; i <= ModelElementCount (model); ++i)
                    if (ElementGuidAt (model, i) == source.terrain) {
                        if (ExtractElementAt (model, i, terrain)) {
                            s_terrain = std::make_shared<const geomsrv::Mesh> (std::move (terrain));
                            s_terrainStamp = source.terrainStamp;
                        }
                        break;
                    }
        }
        if (s_terrain)
            terrain = *s_terrain;
        else if (s_terrainRetries < 5) {
            ++s_terrainRetries;
            s_terrainRetryAt = ::GetTickCount64 () + 500;
            ArchVizLog ("MASSING PYTHON  terrain conversion not ready; retry " + std::to_string (s_terrainRetries));
        }
    }
    API_PlaceInfo place {};
    const bool hasAltitude = ACAPI_GeoLocation_GetPlaceSets (&place) == NoError;
    std::vector<std::string> inputs;
    size_t bytes = 0;
    for (const auto& parcel : parcels) {
        std::string input, error;
        if (!calc::Encode (parcel, terrain, hasAltitude, place.altitude, input, error)) {
            Refuse (error);
            return;
        }
        bytes += input.size ();
        if (bytes > 16 * 1024 * 1024) {
            Refuse ("Combined parcel snapshots exceed the calculation transport budget.");
            return;
        }
        inputs.push_back (std::move (input));
    }
    if (s_worker.joinable ())
        s_worker.join (); // previous worker finished; never join an active calculation here
    s_running.store (true);
    s_page.note = "Calculating with shared Python solver...";
    ArchVizLog ("MASSING PYTHON  started " + source.request.before.guid + " terrain=" + source.terrain);
    const bool missingTerrain = !source.terrain.empty () && terrain.vertices.empty ();
    s_worker = std::thread ([source = std::move (source), inputs = std::move (inputs), missingTerrain] () {
        Completion completion;
        completion.source = source;
        try {
            std::vector<calc::ParcelPreview> previews;
            const auto requests = calc::Expand (source.request);
            const uint64_t began = ::GetTickCount64 ();
            // Reuse the existing bounded input/output bridge. This is a trusted
            // snapshot function, not a command: no UI, ACAPI, global result store
            // or model writes on the Python side, and no new cross-DLL ABI.
            for (size_t i = 0; i < inputs.size (); ++i) {
                const uint64_t elapsed = ::GetTickCount64 () - began;
                if (s_revision.load () != source.generation || elapsed >= 15000) {
                    completion.error = "Parcel calculation superseded or exceeded the site time budget.";
                    break;
                }
                GS::UniString result, bridgeError;
                calc::Result decoded;
                const bool called = evp::PythonHost::Get ().RunGraphScript (
                    "from tapioca.massing.native import preview\npayload = preview(request)\n",
                    "<native Massing calculation>", GS::UniString (inputs[i].c_str (), CC_UTF8),
                    "[{\"portId\":\"payload\"}]", "[]", unsigned (15000 - elapsed), result, bridgeError);
                if (!called)
                    completion.error = bridgeError.ToCStr (0, MaxUSize, CC_UTF8).Get ();
                else
                    calc::Decode (result.ToCStr (0, MaxUSize, CC_UTF8).Get (), decoded, completion.error);
                if (!completion.error.empty ()) {
                    completion.error = "Parcel " + std::to_string (i + 1) + ": " + completion.error;
                    break;
                }
                if (missingTerrain)
                    decoded.note = "Defined terrain has no current 3D surface; check its visibility/3D availability.";
                previews.push_back ({ requests[i], std::move (decoded) });
            }
            if (completion.error.empty ())
                calc::Combine (source.request, std::move (previews), completion.preview, completion.error);
        }
        catch (const std::exception& exception) {
            completion.error = exception.what ();
        }
        {
            std::lock_guard<std::mutex> lock (s_mutex);
            s_completed = std::move (completion);
        }
        s_running.store (false);
        selectionmetadata::Later ([] () {
            Poll (); // adopt on the main thread even when the HUD is idle
            overlayinput::RequestLayout (overlayinput::View::ThreeD);
            overlayinput::RequestLayout (overlayinput::View::Plan);
        });
    });
}
} // namespace

void Request (calc::Request request)
{
    if (!s_queue.Follow (std::move (request), ::GetTickCount64 ()))
        return;
    s_terrainRetryAt = 0;
    s_terrainRetries = 0;
    ClearPublished ();
    const auto& desired = *s_queue.Desired ();
    if (desired.action != calc::Action::Calculate) {
        s_page.note = "Preview paused until inputs change.";
        return;
    }
    s_page.note = "Updating preview...";
    if (s_timer == 0)
        s_timer = ::SetTimer (nullptr, 0, 100, [] (HWND, UINT, UINT_PTR, DWORD) { Poll (); });
}

void Observe ()
{
    if (!s_queue.Desired () || s_queue.Desired ()->action != calc::Action::Calculate)
        return;
    const auto defined = massingmodel::Read ();
    std::string signature;
    for (size_t group : { size_t (0), s_queue.Desired ()->landscape == 0 ? size_t (1) : size_t (2) })
        for (const auto& guid : defined.guids[group])
            signature += guid + ":" + std::to_string (Stamp (guid)) + ";";
    if (signature == s_observed)
        return;
    s_observed = std::move (signature);
    s_terrainRetryAt = 0;
    s_terrainRetries = 0;
    massingmodel::Changed ();
    const auto current = massingmodel::Read ();
    auto desired = *s_queue.Desired ();
    const auto previous = calc::Expand (desired);
    desired.parcels.clear ();
    for (const auto& page : current.parcels) {
        calc::Parcel parcel { page, page.assignments, std::vector<bool> (page.edges.size (), true),
                              std::vector<bool> (page.edges.size (), true) };
        const auto old = std::find_if (previous.begin (), previous.end (),
                                       [&] (const auto& request) { return request.before.guid == page.guid; });
        if (old != previous.end () && old->before.known == page.known &&
            massingrules::SameGeometry (old->before.edges, page.edges) && old->before.hasStored == page.hasStored &&
            old->before.stored == page.stored)
            parcel = { page, old->assignments, old->regulated, old->endpoints };
        desired.parcels.push_back (std::move (parcel));
    }
    desired.before = current.rules;
    if (!desired.parcels.empty ()) {
        desired.assignments = desired.parcels.front ().assignments;
        desired.regulated = desired.parcels.front ().regulated;
        desired.endpoints = desired.parcels.front ().endpoints;
        if (desired.parcels.size () == 1)
            desired.parcels.clear ();
    }
    if (!s_queue.Follow (std::move (desired), ::GetTickCount64 ()))
        s_queue.Refresh (::GetTickCount64 ()); // terrain/roles changed, even if the HUD draft did not
    ClearPublished ();
    Publish (); // synchronize HUD source geometry even when the pointer is idle
}

void BeginReady ()
{
    const auto request = s_queue.TakeReady (::GetTickCount64 (), s_running.load ());
    if (!request)
        return;
    const auto parcels = calc::Expand (*request);
    if (parcels.size () > calc::kMaxParcels ||
        std::any_of (parcels.begin (), parcels.end (), [] (const auto& parcel) { return !parcel.before.known; })) {
        s_page.note = "Define a property-line Polyline to start the automatic preview.";
        return;
    }
    const auto defined = massingmodel::Read ();
    Source source;
    source.request = *request;
    source.terrainSet = defined.guids[request->landscape == 0 ? 1 : 2];
    if (source.terrainSet.size () == 1)
        source.terrain = source.terrainSet[0];
    source.generation = s_queue.Revision ();
    Start (std::move (source));
}

void Poll ()
{
    if (s_polling)
        return;
    s_polling = true;
    Observe ();
    std::optional<Completion> completion;
    {
        std::lock_guard<std::mutex> lock (s_mutex);
        completion.swap (s_completed);
    }
    if (completion && completion->source.generation == s_queue.Revision ()) {
        if (!Current (completion->source))
            Refuse ("Sources or saved rules changed during calculation; stale result discarded.");
        else if (!completion->error.empty ())
            Refuse (completion->error);
        else {
            auto& result = completion->preview.result;
            std::string error = overlaylayers::Validate (result.layer);
            if (!error.empty ())
                Refuse (error);
            else {
                overlaylayers::Set (result.layer);
                overlaylayers::Set (result.site);
                s_source = completion->source;
                s_page.preview = std::make_shared<const calc::Preview> (std::move (completion->preview));
                if (s_terrain && s_terrain->guid == s_source->terrain && s_terrainStamp == s_source->terrainStamp)
                    s_page.terrain = s_terrain;
                s_page.calculated = s_page.preview->result.hasEnvelope;
                if (s_dimensions)
                    overlaylayers::Set (calc::OffsetDimensions (*s_page.preview));
                s_page.note = s_page.preview->result.note;
                ArchVizLog ("MASSING PYTHON  published " + std::to_string (s_page.preview->result.faces) +
                            " planar faces");
                Publish ();
            }
        }
    }
    if (s_terrainRetryAt && ::GetTickCount64 () >= s_terrainRetryAt && !s_running.load () && !s_queue.Pending ()) {
        s_terrainRetryAt = 0;
        s_queue.Refresh (::GetTickCount64 ());
        s_revision.store (s_queue.Revision ());
        // A cold conversion can become available without an element modification.
        // Retry it explicitly; never require the user to move the parcel's Z.
    }
    BeginReady ();
    s_polling = false;
}

Page Read ()
{
    Page page = s_page;
    page.busy = s_running.load () || s_queue.Pending ();
    return page;
}

void Dimensions (bool shown)
{
    if (shown == s_dimensions)
        return;
    s_dimensions = shown;
    overlaylayers::Clear (calc::kDimensions);
    if (shown && s_page.preview)
        overlaylayers::Set (calc::OffsetDimensions (*s_page.preview));
    Publish ();
}

void Forget ()
{
    s_queue.Reset ();
    s_revision.store (s_queue.Revision ());
    if (s_timer != 0) {
        ::KillTimer (nullptr, s_timer);
        s_timer = 0;
    }
    overlaylayers::Clear (kLayer);
    overlaylayers::Clear (kSite);
    overlaylayers::Clear (calc::kDimensions);
    s_dimensions = false;
    s_observed.clear ();
    s_terrain.reset ();
    s_terrainRetryAt = 0;
    s_terrainRetries = 0;
    s_source.reset ();
    s_page = {};
}

void Shutdown ()
{
    Forget ();
    if (s_worker.joinable ())
        s_worker.join ();
    std::lock_guard<std::mutex> lock (s_mutex);
    s_completed.reset ();
}
} // namespace geomsrv::archviz::massinghybrid
