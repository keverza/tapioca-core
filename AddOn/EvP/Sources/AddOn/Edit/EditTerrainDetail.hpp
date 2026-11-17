#ifndef EVP_EDIT_EDITTERRAINDETAIL_HPP
#define EVP_EDIT_EDITTERRAINDETAIL_HPP

// The built terrain at a level of detail (Mesh detail). The build refines to a centimetre so its
// volumes and its certification are exact; that mesh stays the authority (PublishedTerrain). What
// is shown and what is baked into an Archicad Mesh is this one: far fewer triangles, with
//
//   exact     the existing ground's own vertices where the design left it, fixed-height
//             controls, the paths' design lines (band edges, shoulders, ends, daylight) and the
//             regions' and property's lines, and the boundary -- lines reduced only where they
//             stay within 1 mm;
//   within    every other built vertex within `tolerance` metres in height.
//
// It is a constrained Delaunay triangulation of its points and lines, so an Archicad Mesh made
// from the same points (level points) and lines (level lines) is the same surface.
#include "Edit/EditPublishedTerrain.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace tapioca::geometry {

// The choices offered, metres; zero is the full build.
constexpr double kMeshDetailFull = 0.0;
constexpr double kMeshDetailFine = 0.01;
constexpr double kMeshDetailStandard = 0.05;
constexpr double kMeshDetailCoarse = 0.10;

struct DetailedTerrain {
    std::vector<double> points; // xyz
    std::vector<uint32_t> triangles;
    std::vector<uint32_t> source;               // per point, its built vertex
    std::vector<std::array<uint32_t, 2>> lines; // kept edges (lines and boundary), point indices
    double tolerance = 0.0;
    uint64_t revision = 0; // the published terrain's
    size_t builtTriangles = 0;
};

// The published terrain's exact features (see the header): its kept vertices and line edges.
struct DetailFeatures {
    std::vector<uint32_t> keep;
    std::vector<uint32_t> lines; // flat vertex pairs
};
DetailFeatures PublishedDetailFeatures (const PublishedTerrain& published, const std::vector<double>& baselinePoints);

// `tolerance` > 0. False with the reason when the surface could not be made.
bool DetailPublishedTerrain (const PublishedTerrain& published, const std::vector<double>& baselinePoints,
                             double tolerance, DetailedTerrain& detailed, std::string& error);

} // namespace tapioca::geometry
#endif
