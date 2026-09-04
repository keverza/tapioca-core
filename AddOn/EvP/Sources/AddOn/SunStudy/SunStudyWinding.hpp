#ifndef EVP_SUNSTUDY_SUNSTUDYWINDING_HPP
#define EVP_SUNSTUDY_SUNSTUDYWINDING_HPP

// SunStudy/SunStudyWinding — proving which way a face points before anything
// measures it.
//
// ⚠️ EVERY ANSWER THIS ENGINE GIVES RESTS ON THE FACE NORMAL, and nothing
// upstream guarantees which way it points. The occlusion pass refuses a sample
// whose normal fails `dot(n, sunDir) > 0`; the sampler lifts each sample ALONG
// its normal to escape self-shadowing. So an element wound the other way reports
// its sunlit face as permanently dark AND pushes every sample INTO the solid --
// two symptoms, one cause, and neither of them looks like a winding problem.
// It shows up as a plausible study of a building that happens to be in shade.
//
// ⚠️ WINDING IS CONSISTENT ONLY WITHIN AN ELEMENT, never across the merged
// snapshot, so the test is per group and it is a PROOF rather than a heuristic:
//
//   * the element must be CLOSED -- every undirected edge shared by exactly two
//     triangles. An open surface has no inside, so "outward" is undefined and
//     there is nothing to prove;
//   * for a closed surface the divergence theorem gives the enclosed volume as
//     sum(dot(v0, cross(v1, v2))) / 6, whose SIGN is the winding. Negative means
//     the normals point inward.
//
// Anything failing either test is left exactly as it came. ⚠️ A WRONG FLIP IS
// FAR WORSE THAN A MISSED ONE -- it inverts a surface that was already right --
// so the bar is a closed manifold and nothing less.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace evp::sunstudy {

struct WindingReport {
    size_t groups = 0;           // distinct element ids seen
    size_t closed = 0;           // groups that proved closed, so were testable
    size_t flipped = 0;          // groups whose volume came out negative
    size_t flippedTriangles = 0; // triangles whose winding was swapped
};

// Returns a copy of `triangles` with inward-wound groups' winding swapped.
// `groups` is one element id per face, or empty for "all one element".
//
// ⚠️ ADJACENCY IS A QUESTION ABOUT POSITIONS, NOT INDICES. The snapshot merges
// elements without sharing vertices, so two triangles meeting along an edge
// generally hold four distinct indices for its two corners. Comparing indices
// would find no shared edges at all and every element would read as open --
// which fails safe, and therefore silently never flips anything.
std::vector<uint32_t> OrientOutward (const double* vertices, size_t vertexCount, const uint32_t* triangles,
                                     size_t faceCount, const uint32_t* groups, WindingReport& report,
                                     double weldTolerance = 1e-4);

// The signed volume of a closed triangle soup, by the divergence theorem.
//
// ⚠️ VERTICES ARE CENTRED BEFORE THE SUM AND THAT IS NOT TIDINESS. At national
// grid coordinates the triple product runs to ~1e17 while the volume it is meant
// to reveal is ~1e2; float64 cancellation then makes the SIGN noise, and the
// sign is the entire answer. Centring costs one subtraction and is the
// difference between a proof and a coin flip.
double SignedVolume (const double* vertices, const uint32_t* triangles, const uint32_t* faceList, size_t faceCount);

} // namespace evp::sunstudy

#endif // EVP_SUNSTUDY_SUNSTUDYWINDING_HPP
