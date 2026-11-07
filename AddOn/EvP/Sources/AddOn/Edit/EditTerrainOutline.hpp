#ifndef TAPIOCA_EDIT_TERRAIN_OUTLINE_HPP
#define TAPIOCA_EDIT_TERRAIN_OUTLINE_HPP

// The terrain graded from the paved area of every path (the user, 2026-10-24: grade the
// terrain from the paved outline of all paths). Inside the area the ground is the paved
// surface itself -- every path, junction and kerb fillet as paved; outside it, faces fall
// or rise from the outline to the ground, rounded at top and toe. Only where the terrain
// is, inside the property line: a path may run past both and is graded inside them.
//
// This file only builds the plan geometry -- the paved outline clipped to that area, rings
// offset from it for the faces to lie on, the region they span -- and hands it with the
// paved surface to the Rust kernel (BuildOutlineTerrain), which owns every height and volume.
#include "Edit/EditTerrainDesign.hpp"

namespace tapioca::geometry {

struct OutlineDesignOptions {
    TerrainSlope cut { TerrainSlope::Unit::Percent, 50, 0 };
    TerrainSlope fill { TerrainSlope::Unit::Percent, 33, 0 };
    double limit = 10.0;     // Plan reach of a face from the outline, metres.
    double rounding = 1.0;   // Plan metres each face rounds over at its top and toe.
    double margin = 1.0;     // Beyond the reach, metres, before the region's edge.
    double pavedStep = 0.5;  // The paved surface's sampling, metres.
    double tolerance = 1e-3; // At-grade height band, metres.
    TerrainEditEffect effect = TerrainEditEffect::Both;
};

// The grading options a document's path asks for: its first cross-section range's cut and
// fill slopes and rounding, and the reach given. These grade only a paved vertex no path can
// be found for: every other face takes the slopes and rounding of the path section it was
// paved from (#151).
OutlineDesignOptions OutlineOptionsFor (const geomsrv::edit::Document& doc, geomsrv::edit::Id path, double limit);

// The plan geometry one outline grading is built from (#149 T1: shared by this lane and the
// unified build, so both grade exactly the same inputs): the paved surface of every path and
// its per-vertex faces, the open face lines inside the region, the paved interior vertices as
// spots, and the region ring -- the faces' reach and margin around the paved area, on the
// terrain inside the property line, one piece. All plan metres; xyz triples.
struct OutlineGeometry {
    std::vector<double> surfacePoints;
    std::vector<uint32_t> surfaceTriangles;
    std::vector<double> surfaceGrades; // (cut, fill, rounding) per paved vertex; empty: uniform.
    std::vector<std::vector<double>> lines;
    std::vector<double> spots;
    std::vector<double> region;
};
// Refuses OutlineNoPaths / OutlineOutside as EvaluateOutlineDesign does; `out` unchanged then.
bool BuildOutlineGeometry (const geomsrv::edit::Document& doc, const TerrainMeshView& mesh,
                           const OutlineDesignOptions& options, OutlineGeometry& out, std::string& error);

// Worker-only: grades `input`'s terrain from every path of `doc`. `result.build` carries the
// proposed points, triangles, volumes and the region ring's vertices (mapping rows 17 only:
// a preview, no support provenance); `result.region` the ring. `unreached` counts vertices
// whose face ran out at the limit before meeting the ground; `clipped`, when given, the region
// ring samples where a face ends in a step at the region's edge (a wider limit cannot help
// those). Refuses with a code: OutlineNoPaths, OutlineOutside, Outline*/Region* from the
// kernel. `result` is unchanged on refusal.
bool EvaluateOutlineDesign (const geomsrv::edit::Document& doc, const EditTerrainInput& input,
                            const OutlineDesignOptions& options, TerrainDesignResult& result, uint32_t& unreached,
                            std::string& error, uint32_t* clipped = nullptr);

} // namespace tapioca::geometry
#endif
