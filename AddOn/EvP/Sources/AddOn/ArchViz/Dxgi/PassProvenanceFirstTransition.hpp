#ifndef EVP_ARCHVIZ_DXGI_PASSPROVENANCEFIRSTTRANSITION_HPP
#define EVP_ARCHVIZ_DXGI_PASSPROVENANCEFIRSTTRANSITION_HPP

// ArchViz/Dxgi/PassProvenanceFirstTransition -- first Known->Ambiguous non-camera draw latch, no behaviour change.
// Bound by private/docs/architecture/diligent/OVERLAY-INVARIANTS.md: stores only what PassProvenance.cpp hands it.

#include "ArchViz/Dxgi/PassProvenance.hpp"

#include <atomic>
#include <cstdint>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace passprovenance {
namespace firsttransition {

// CONTEXT THREAD, called from PassProvenance.cpp::ClearContextState. Clears the
// latch so a later run does not report a capture from a superseded generation.
void Reset ();

// CONTEXT THREAD, called from PassProvenance.cpp::OnDrawCompleted once per reset
// generation. `contextResetApplied` is PassProvenance.cpp's own g_contextResetApplied,
// passed by reference and read here exactly where the original body read it: as the
// source of the final release store. The caller is responsible for calling this at
// most once per generation -- this function does not itself gate on "already captured".
void Publish (const FirstKnownToAmbiguousDraw& draw, const std::atomic<uint64_t>& contextResetApplied);

// MAIN THREAD, called from PassProvenance.cpp::GetStats. `resetRequested` and
// `contextResetApplied` are PassProvenance.cpp's own g_resetRequested /
// g_contextResetApplied, passed by reference so every load below happens against the
// live atomics -- this module owns neither and never caches a snapshot of them.
FirstKnownToAmbiguousDraw Read (const std::atomic<uint64_t>& resetRequested,
                                const std::atomic<uint64_t>& contextResetApplied);

} // namespace firsttransition
} // namespace passprovenance
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
