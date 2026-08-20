#ifndef GEOMETRYSERVER_METADATAEXTRACTOR_HPP
#define GEOMETRYSERVER_METADATAEXTRACTOR_HPP

#include "MetadataStore.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// ACAPI — MAIN THREAD ONLY.
//
// COST WARNING (this froze Archicad on a large project once):
//   * Walking EVERY element in the project (`API_ZombieElemID`) is wasteful — a
//     model with 874 meshes had 15,342 elements. We now gather metadata only for
//     the GUIDs actually in the snapshot.
//   * Properties are the expensive part: GetPropertyDefinitions +
//     GetPropertyValues + GetPropertyValueString **per element**. On a big model
//     that is millions of main-thread ACAPI calls. So properties/classifications
//     are a separate, opt-in level.
//
// `shouldCancel` is polled periodically; return true to abort (returns nullptr).
// `onProgress(done, total)` is called periodically for the progress bar.
namespace geomsrv {

enum class MetaLevel {
    None,     // nothing
    Basic,    // guid, type name, element ID, layer, story   — cheap
    Full,     // + classifications + user-defined properties — EXPENSIVE
};

using CancelFn   = std::function<bool ()>;
using ProgressFn = std::function<void (size_t done, size_t total)>;

// Metadata for exactly these elements (typically the snapshot's mesh GUIDs).
// GUIDs that are not API elements (composite sub-parts) are skipped.
std::shared_ptr<const MetaSet> ExtractMetadataFor (const std::vector<std::string>& guids,
                                                   MetaLevel level,
                                                   const CancelFn& shouldCancel = nullptr,
                                                   const ProgressFn& onProgress = nullptr);

} // namespace geomsrv

#endif
