#ifndef EVP_GRASSHOPPER_GHWORKERDESCRIPTION_HPP
#define EVP_GRASSHOPPER_GHWORKERDESCRIPTION_HPP

#include "HostState.hpp"
#include "UniString.hpp"

#include <cstdint>
#include <string>

namespace evp {
namespace grasshopper {

class GhBridge;
namespace protocol {
struct RunReportPayload;
}

// Formats an already captured lifecycle/bridge state. Does not own or mutate
// the worker; keeping the presentation outside the supervisor avoids growing
// another branch in its process and cancellation logic.
GS::UniString DescribeWorker (HostState state, uint32_t generation, PeerOwnership ownership, bool gh2,
                              uint32_t archicadPort, const GS::UniString& workerPath, const GS::UniString& lastMessage,
                              const std::string& failure, const GhBridge& bridge);
GS::UniString DescribeRunResult (const protocol::RunReportPayload& report);

} // namespace grasshopper
} // namespace evp

#endif
