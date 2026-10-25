#ifndef EVP_NATIVECOMMANDS_SUNSTUDYHUDRUNNER_HPP
#define EVP_NATIVECOMMANDS_SUNSTUDYHUDRUNNER_HPP

// The viewer HUD's "Run sun study": the same command sequence the palette's
// Native sun study runs (pause the follower, snapshot, start, advance in
// slices, show), driven natively so the study needs no Python command.
//
// ⚠️ PLAIN TYPES ONLY IN THIS HEADER. The HUD (render thread, ArchViz) includes
// it; the DevKit and the command registry stay in the .cpp.
//
// Submit/Cancel/Current are safe from any thread. The run itself is on its own
// thread; commands that need the host main thread go through MainThreadGate.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::sunstudyhud {

struct Request {
    int year = 2026, month = 3, day = 21;
    int timestep = 15; // minutes: 15, 30 or 60
    double grid = 1.0; // cell size, metres
    // The palette command's roles: Context casts shadow and is not measured,
    // Ignored is absent; everything else is analysed.
    std::vector<std::string> contextElements;
    std::vector<std::string> ignoredElements;
    // The size safeguard (SunStudy/SunStudyBounds.hpp): only elements reaching
    // into this plan circle are measured; the rest still cast shadow.
    bool bounded = false;
    double centreX = 0.0, centreY = 0.0, radius = 0.0;
};

struct Status {
    bool running = false;
    std::string stage; // "snapshot", "starting", "calculating", "showing", "done", "cancelled", "failed"
    size_t resolvedSteps = 0;
    size_t totalSteps = 0;
    std::string studyId;
    std::string error;
    double wallMs = 0.0;
    uint64_t runs = 0;
};

bool Submit (const Request& request, std::string& error);
void Cancel ();
Status Current ();
// Quit/unload: cancel and join.
void Shutdown ();

} // namespace geomsrv::sunstudyhud

#endif
