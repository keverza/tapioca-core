#ifndef GEOMETRYSERVER_PLANARTRIANGULATION_HPP
#define GEOMETRYSERVER_PLANARTRIANGULATION_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv::engine {

// How the polygon was covered. This deliberately does not expose earstrip types.
enum class PlanarCover : uint8_t {
    Exact,       // The triangles cover the rings exactly.
    Repaired,    // Constrained Delaunay cover, used for repair or many rings.
    Approximate, // Forced cuts; suitable only for preview callers.
    Refused,     // No reliable cover.
    HoleDropped, // A hole could not be preserved; always a caller error.
};

// A borrowed ring of x,y pairs. rings[0] is the outer ring; rings[1..] are holes.
struct PlanarRing {
    const double* xy = nullptr;
    size_t count = 0;
};

struct PlanarTriangulation {
    // Three CCW indices per triangle, into the concatenated raw ring vertices.
    std::vector<uint32_t> triangles;
    // Optional strip view over the same vertices; empty when not requested.
    std::vector<uint32_t> strips;
    std::vector<uint32_t> stripEnds;
    std::vector<uint8_t> stripClockwise;
    // Crossing points added in Repaired mode: index (input vertex count + k).
    std::vector<double> generatedXY;
    PlanarCover cover = PlanarCover::Refused;
};

// Never guesses a ring hierarchy. Returns false and leaves out untouched on
// refusal; error names the contract that failed, not the library's internals.
// Exact, Repaired and Approximate are successful covers; callers choose their
// own repair/approximation policy from cover rather than parsing error.
// Rings must already be one outer plus contained, non-overlapping holes; this
// is not a region union or hierarchy builder. Zero-area holes refuse. Either
// winding and repeated closing points are accepted without changing raw indices.
// Exact covers and all covers with holes must match |outer|-sum(|holes|) within
// 1e-9 relative area. A repaired single outer instead uses non-zero winding (its
// shoelace area need not equal its filled area). Predicates use a span-relative
// grid; this is not the exact-lattice surface-coverage or terrain TIN path.
bool TriangulatePlanar (const std::vector<PlanarRing>& rings, bool wantStrips, PlanarTriangulation& out,
                        std::string& error);

// Frees the calling thread's scratch after a very large polygon.
void ShrinkPlanarTriangulationScratch ();

} // namespace geomsrv::engine
#endif
