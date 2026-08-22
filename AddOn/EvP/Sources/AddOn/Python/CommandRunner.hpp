#ifndef EVP_COMMANDRUNNER_HPP
#define EVP_COMMANDRUNNER_HPP

#include "UniString.hpp"
#include "Array.hpp"

#include <cstdint>
#include <functional>

namespace evp {

struct CommandRunSpec {
    GS::UniString path, module, paramsJson, title, folderDir, runtimeHome, packageDir;
    // Empty for an ordinary run; the name of a declared output action otherwise.
    GS::UniString action;
    // Empty unless that action came from the palette's right-click menu, in which
    // case it is where the click landed — the command reads it as `ctx.region`.
    GS::UniString menuRegion;
    GS::UniString requiresApi, requiresTapir;
    GS::Array<GS::UniString> requirements;
    bool external = false;
    unsigned short port = 0;
    uint64_t generation = 0;
    std::function<void (uint64_t, const GS::UniString&)> finish;
};

// Starts the Zone B/C worker. Requirement checks happen before dependency setup or
// command import, so an unmet Tapioca/Tapir floor cannot execute user code.
void StartCommandRun (CommandRunSpec spec);

} // namespace evp

#endif
