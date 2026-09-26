#ifndef TAPIOCA_PALETTE_WORKFLOWATTACHENGINE_HPP
#define TAPIOCA_PALETTE_WORKFLOWATTACHENGINE_HPP

#include <optional>

namespace evp::palette {

// true = GH2, false = GH1, nullopt = dismissed without changing the bridge.
std::optional<bool> ChooseWorkflowAttachEngine ();

} // namespace evp::palette

#endif
