#include "NativeCommands/SurfaceTakeoffSeedInput.hpp"
#include "Geometry/SurfaceTakeoffController.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace geomsrv::surfacetakeoff {
namespace {
struct OwnedPick {
    const SeedPicker& picker;
    std::string id;

    ~OwnedPick ()
    {
        if (!id.empty ())
            picker.cancel (id);
    }
};
} // namespace

SeedInputResult CollectExteriorSeeds (const Request& request, size_t count, const SeedPicker& picker)
{
    using polygonpick::Status;
    const polygonpick::WorldIdentity expected { request.captureStamp, true, request.snapshotId };
    std::vector<Seed> seeds;
    for (size_t i = 0; i < count; ++i) {
        if (picker.interrupted ())
            return { {}, "Exterior seed input cancelled.", true };
        const std::string prompt = "Pick EXTERIOR facade " + std::to_string (i + 1) + "/" + std::to_string (count);
        OwnedPick pick { picker, {} };
        std::string error;
        if (!picker.start (prompt, pick.id, error))
            return { {}, std::move (error) };

        const auto deadline = std::chrono::steady_clock::now () + std::chrono::minutes (2);
        polygonpick::Outcome outcome;
        for (;;) {
            if (picker.interrupted ())
                return { {}, "Exterior seed input cancelled.", true };
            outcome = picker.poll (pick.id);
            if (outcome.status != Status::Pending && outcome.status != Status::Running)
                break;
            if (std::chrono::steady_clock::now () >= deadline)
                return { {}, "Exterior pick timed out. Press Esc if Archicad still shows the input prompt." };
            picker.wait ();
        }
        pick.id.clear (); // terminal outcome was consumed
        if (picker.interrupted ())
            return { {}, "Exterior seed input cancelled.", true };
        if (outcome.status == Status::Cancelled)
            return { {}, "Exterior pick cancelled: " + outcome.reason, true };
        if (outcome.status != Status::Done)
            return { {}, outcome.reason.empty () ? "The exterior pick is no longer active." : outcome.reason };
        const auto& face = outcome.pick;
        if (outcome.expected != expected || !face.found || !outcome.callbackMatchedPick || !face.haveNormal)
            return { {}, "Pick has no matching callback/capture. Refresh and pick inside the exterior face." };
        const auto finite = [] (double value) { return std::isfinite (value); };
        if (!std::all_of (std::begin (face.pos), std::end (face.pos), finite) ||
            !std::all_of (std::begin (face.normal), std::end (face.normal), finite))
            return { {}, "Non-finite picked point or normal." };
        const auto guid = CanonicalGuid (face.guid);
        if (std::none_of (request.targetGuids.begin (), request.targetGuids.end (),
                          [&guid] (const std::string& target) { return CanonicalGuid (target) == guid; }))
            return { {}, "Picked face is outside the captured wall targets: " + guid };
        // Picker database ordinals never cross into ModelerAPI source identity.
        seeds.push_back (
            { guid, { face.pos[0], face.pos[1], face.pos[2] }, { face.normal[0], face.normal[1], face.normal[2] } });
    }
    return { std::move (seeds), {} };
}

} // namespace geomsrv::surfacetakeoff
