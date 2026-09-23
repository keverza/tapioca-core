// Tests for SunStudy/SunStudyReading -- the hover inspector's value under a
// point.
//
// ⚠️ THE CLAIM IS AGREEMENT WITH THE PICTURE. The inspector exists to say what
// the colour under the cursor means; if it read a different cell from the one
// the tint shader drew, it would disagree exactly at cell boundaries and be
// believed everywhere else. So the decisive test here drives the REAL display
// records through the shader's own arithmetic and requires the same sample.

#include <cmath>
#include <string>
#include <vector>

#include "ArchViz/SunStudyOverlay.hpp"
#include "SunStudy/SunStudyPatchAtlas.hpp"
#include "SunStudy/SunStudyReading.hpp"
#include "SunStudy/SunStudyStore.hpp"
#include "gtest/gtest.h"

using namespace evp::sunstudy;

namespace {

// A 6 m x 2 m wall as 12 welded triangles (one surface), plus a 4 m x 3 m quad
// of a second element.
struct Scene {
    std::vector<double> vertices;
    std::vector<uint32_t> triangles;
    std::vector<uint32_t> groups;
    std::vector<std::string> elementOf { "wall", "slab" };

    Scene ()
    {
        for (int i = 0; i <= 6; ++i) {
            vertices.insert (vertices.end (), { double (i), 0.0, 0.0 });
            vertices.insert (vertices.end (), { double (i), 2.0, 0.0 });
        }
        for (uint32_t i = 0; i < 6; ++i) {
            const uint32_t a = i * 2, b = i * 2 + 1, c = i * 2 + 2, d = i * 2 + 3;
            triangles.insert (triangles.end (), { a, c, d, a, d, b });
            groups.insert (groups.end (), { 0u, 0u });
        }
        const uint32_t base = static_cast<uint32_t> (vertices.size () / 3);
        vertices.insert (vertices.end (), { 20, 0, 0, 24, 0, 0, 24, 3, 0, 20, 3, 0 });
        triangles.insert (triangles.end (), { base, base + 1, base + 2, base, base + 2, base + 3 });
        groups.insert (groups.end (), { 1u, 1u });
    }
    size_t Faces () const
    {
        return triangles.size () / 3;
    }
};

// Distinct hours per sample, so "the right sample" is checkable by value.
std::vector<double> Hours (size_t count)
{
    std::vector<double> hours (count);
    for (size_t i = 0; i < count; ++i)
        hours[i] = 0.25 * double (i);
    return hours;
}

PatchSampleGrid Patches (const Scene& scene, double spacing, const std::vector<uint8_t>* mask = nullptr)
{
    PatchSamplerOptions options;
    options.spacing = spacing;
    options.normalOffset = 0.0;
    options.sampleGroup = mask;
    return BuildPatchSampleGrid (scene.vertices.data (), scene.vertices.size () / 3, scene.triangles.data (),
                                 scene.Faces (), scene.groups.data (), scene.elementOf, options);
}

} // namespace

TEST (SunStudyReading, EveryPatchSampleReadsItselfFromEveryTriangleOfItsSurface)
{
    const Scene scene;
    const PatchSampleGrid grid = Patches (scene, 0.7); // does not divide either rectangle
    ASSERT_TRUE (grid.valid);
    const std::vector<double> hours = Hours (grid.Count ());

    for (size_t sample = 0; sample < grid.Count (); ++sample) {
        const double point[3] = { grid.positions[sample * 3], grid.positions[sample * 3 + 1],
                                  grid.positions[sample * 3 + 2] };
        for (size_t face = 0; face < scene.Faces (); ++face) {
            if (grid.patchOfTriangle[face] != grid.spanOf[sample])
                continue;
            const SunStudyReading reading = ReadPatchStudyAt (grid, 0.7, hours, face, point);
            ASSERT_TRUE (reading.measured);
            EXPECT_EQ (reading.sample, sample) << "through face " << face;
            EXPECT_FALSE (reading.nearest);
            EXPECT_DOUBLE_EQ (reading.hours, hours[sample]);
        }
    }
}

TEST (SunStudyReading, EveryTriangleSampleReadsItself)
{
    const Scene scene;
    SamplerOptions options;
    options.spacing = 0.7;
    options.normalOffset = 0.0;
    options.wantLayouts = true;
    const SampleGrid grid = BuildSampleGrid (scene.vertices.data (), scene.vertices.size () / 3,
                                             scene.triangles.data (), scene.Faces (), scene.groups.data (), options);
    ASSERT_TRUE (grid.valid);
    const std::vector<double> hours = Hours (grid.Count ());

    for (size_t sample = 0; sample < grid.Count (); ++sample) {
        const double point[3] = { grid.positions[sample * 3], grid.positions[sample * 3 + 1],
                                  grid.positions[sample * 3 + 2] };
        const SunStudyReading reading = ReadTriangleStudyAt (grid, 0.7, hours, grid.faces[sample], point);
        ASSERT_TRUE (reading.measured);
        EXPECT_EQ (reading.sample, sample);
        EXPECT_DOUBLE_EQ (reading.hours, hours[sample]);
    }
}

TEST (SunStudyReading, TheReaderAndTheShaderReadTheSameCellAcrossTheSurface)
{
    // ⚠️ THE DECISIVE ONE. A dense walk of points over the wall, including cell
    // boundaries: wherever the reader found the cell's own sample, the shader's
    // arithmetic on the real display record lands on that sample's texel.
    const Scene scene;
    const PatchSampleGrid grid = Patches (scene, 0.7);
    SunStudyPatchAtlas atlas;
    atlas.Fit (grid);
    std::vector<AtlasTile> tiles;
    std::vector<FaceLayout> layouts;
    ASSERT_TRUE (PatchFaceArrays (grid, atlas, tiles, layouts));
    geomsrv::archviz::SunStudyElementMap map;
    const std::vector<int32_t> material (scene.Faces (), 0); // identity draw order
    ASSERT_TRUE (geomsrv::archviz::BuildSunStudyElementMap (tiles, layouts, 0.7, scene.triangles, material, 0, map));
    const std::vector<double> hours = Hours (grid.Count ());

    size_t checked = 0;
    for (double x = 0.01; x < 6.0; x += 0.13) {
        for (double y = 0.01; y < 2.0; y += 0.11) {
            const double point[3] = { x, y, 0.0 };
            const size_t face = 0; // any triangle of the wall: one record for all
            const SunStudyReading reading = ReadPatchStudyAt (grid, 0.7, hours, face, point);
            ASSERT_TRUE (reading.measured);
            if (reading.nearest)
                continue;
            const int64_t shaderTexel =
                geomsrv::archviz::SunStudyTexelAt (map.faces[face], point, atlas.Width (), atlas.Height ());
            EXPECT_EQ (shaderTexel, atlas.TexelOf (grid, reading.sample)) << "at " << x << ", " << y;
            ++checked;
        }
    }
    EXPECT_GT (checked, 500u);
}

TEST (SunStudyReading, AContextSurfaceHasNoMeasurement)
{
    const Scene scene;
    const std::vector<uint8_t> analysed { 1u, 0u }; // the slab is context
    const PatchSampleGrid grid = Patches (scene, 1.0, &analysed);
    const double point[3] = { 21.0, 1.0, 0.0 };
    const SunStudyReading reading = ReadPatchStudyAt (grid, 1.0, Hours (grid.Count ()), 12, point);
    EXPECT_FALSE (reading.measured) << "a context surface reported some other surface's hours";
}
