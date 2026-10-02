// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.

// ArchViz/ExtractionReport -- what an extraction pass SAYS when it finishes.
//
// ⚠️ EXTRACTED BECAUSE THE PASS MAY NOT GROW. Its OVERSIZED
// entry says so in as many words: "this entry freezes its size; the next feature
// extracts a seam rather than growing it". Reporting is that seam -- the pass
// decides what happened, this decides what a reader is told -- and it is the same
// seam already cut for the overlay runtime.

#ifndef GEOMSRV_ARCHVIZ_EXTRACTIONREPORT_HPP
#define GEOMSRV_ARCHVIZ_EXTRACTIONREPORT_HPP

#include "ArchViz/ExtractionThread.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace geomsrv {
namespace archviz {
namespace extractionreport {

// One pass, one report. `elementsSeen` is what the model offered -- the filter
// size on a partial pass, the model count on a full one, which only the caller
// knows.
void Pass (const ExtractionWorker::Progress& progress, bool partial, size_t elementsSeen, uint32_t removed);

// The three lists a pass's empty elements are told in, pure so tests/cpp pins them:
// - ORDINARY: 2D kinds, and `+`-prefixed parts with nothing of their own to draw;
// - CARRIED: elements whose geometry is ANOTHER element's -- an opening's (window, door,
//   skylight, opening) is its host wall's or roof's, a container's (column, beam, stair,
//   railing, curtain wall) is its parts', each of which the model lists with its own GUID;
// - GAP: every other kind -- solid elements the overlay is not drawing -- each followed by
//   why, the commonest reason first: `object x3 (no bodies x3)`.
// One reason is a gap whatever the kind: `past the model's end`, the model changed under the
// pass and the element was never read.
struct EmptyLists {
    std::string ordinary;
    std::string carried;
    std::string gaps;
};
EmptyLists DescribeEmpty (const std::map<std::string, uint32_t>& byType,
                          const std::map<std::string, std::map<std::string, uint32_t>>& reasons);

// The notice a single-pass live sync owes its reader. See ExtractionThread for
// why observers are off: attaching them writes to the project database.
void SinglePassNotice ();

// What the building-material vote resolved.
void SubstanceJoin (size_t named, size_t surfaces, uint32_t classified, uint32_t total, size_t observations,
                    const std::string& error);

} // namespace extractionreport
} // namespace archviz
} // namespace geomsrv

#endif
