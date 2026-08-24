#include "ArchViz/PointCloudPly.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

using namespace geomsrv::archviz;

namespace {

template <typename T> void Append (std::string& bytes, T value)
{
    const char* raw = reinterpret_cast<const char*> (&value);
    bytes.append (raw, sizeof (value));
}

std::istringstream BinaryInput (std::string bytes)
{
    return std::istringstream (std::move (bytes), std::ios::in | std::ios::binary);
}

TEST (PointCloudPly, LoadsDoubleCoordinatesColoursNormalsAndScalarMetadata)
{
    std::string bytes =
        "ply\nformat binary_little_endian 1.0\n"
        "comment global_shift_x -500000\ncomment global_shift_y -6000000\ncomment global_shift_z 0\n"
        "element vertex 2\nproperty double x\nproperty double y\nproperty double z\n"
        "property uchar red\nproperty uchar green\nproperty uchar blue\n"
        "property float nx\nproperty float ny\nproperty float nz\nproperty float scalar_Classification\n"
        "end_header\n";
    Append (bytes, 500000.25);
    Append (bytes, 6000000.5);
    Append (bytes, 12.0);
    Append<uint8_t> (bytes, 10);
    Append<uint8_t> (bytes, 20);
    Append<uint8_t> (bytes, 30);
    Append (bytes, 0.0f);
    Append (bytes, 0.0f);
    Append (bytes, 1.0f);
    Append (bytes, 2.0f);
    Append (bytes, 500001.25);
    Append (bytes, 6000002.5);
    Append (bytes, 15.0);
    Append<uint8_t> (bytes, 40);
    Append<uint8_t> (bytes, 50);
    Append<uint8_t> (bytes, 60);
    Append (bytes, 1.0f);
    Append (bytes, 0.0f);
    Append (bytes, 0.0f);
    Append (bytes, 5.0f);

    auto input = BinaryInput (std::move (bytes));
    PointCloudData cloud;
    std::string error;
    ASSERT_TRUE (LoadPointCloudPly (input, cloud, error)) << error;
    ASSERT_EQ (cloud.vertices.size (), 2u);
    EXPECT_DOUBLE_EQ (cloud.origin[0], 500000.25);
    EXPECT_DOUBLE_EQ (cloud.origin[1], 6000000.5);
    EXPECT_FLOAT_EQ (cloud.vertices[1].position[0], 1.0f);
    EXPECT_FLOAT_EQ (cloud.vertices[1].position[1], 2.0f);
    EXPECT_FLOAT_EQ (cloud.vertices[1].position[2], 3.0f);
    EXPECT_EQ (cloud.vertices[0].rgba, 0xFF1E140Au);
    EXPECT_TRUE (cloud.hasColours);
    EXPECT_TRUE (cloud.hasNormals);
    EXPECT_TRUE (cloud.hasReportedGlobalShift);
    EXPECT_DOUBLE_EQ (cloud.reportedGlobalShift[0], -500000.0);
    ASSERT_EQ (cloud.scalarFields.size (), 1u);
    EXPECT_EQ (cloud.scalarFields[0], "Classification");
}

TEST (PointCloudPly, LoadsFloatXyzWithDefaultWhite)
{
    std::string bytes = "ply\nformat binary_little_endian 1.0\nelement vertex 1\n"
                        "property float z\nproperty float x\nproperty float y\nend_header\n";
    Append (bytes, 3.0f);
    Append (bytes, 1.0f);
    Append (bytes, 2.0f);
    auto input = BinaryInput (std::move (bytes));
    PointCloudData cloud;
    std::string error;
    ASSERT_TRUE (LoadPointCloudPly (input, cloud, error)) << error;
    EXPECT_EQ (cloud.vertices[0].rgba, 0xFFFFFFFFu);
    EXPECT_FALSE (cloud.hasColours);
    EXPECT_FALSE (cloud.hasNormals);
}

TEST (PointCloudPly, RejectsUnsupportedAndTruncatedInput)
{
    PointCloudData cloud;
    std::string error;
    auto ascii = BinaryInput ("ply\nformat ascii 1.0\nelement vertex 0\nend_header\n");
    EXPECT_FALSE (LoadPointCloudPly (ascii, cloud, error));
    EXPECT_NE (error.find ("binary_little_endian"), std::string::npos);

    auto truncated = BinaryInput ("ply\nformat binary_little_endian 1.0\nelement vertex 1\n"
                                  "property float x\nproperty float y\nproperty float z\nend_header\n");
    EXPECT_FALSE (LoadPointCloudPly (truncated, cloud, error));
    EXPECT_NE (error.find ("truncated"), std::string::npos);
}

TEST (PointCloudPly, PlacesGlobalMetresRelativeToArchicadSurveyCoordinates)
{
    std::string bytes = "ply\nformat binary_little_endian 1.0\nelement vertex 2\n"
                        "property double x\nproperty double y\nproperty double z\nend_header\n";
    Append (bytes, 580083.25);
    Append (bytes, 6061795.5);
    Append (bytes, 120.0);
    Append (bytes, 580084.25);
    Append (bytes, 6061797.5);
    Append (bytes, 123.0);
    auto input = BinaryInput (std::move (bytes));
    PointCloudData cloud;
    std::string error;
    ASSERT_TRUE (LoadPointCloudPly (input, cloud, error)) << error;

    // Project -> Survey: the Archicad project origin is at the real-world
    // easting/northing. API_Tranmat and API_Coord3D are expressed in metres.
    const double projectToSurvey[12] = {
        1.0, 0.0, 0.0, 580000.0,
        0.0, 1.0, 0.0, 6062000.0,
        0.0, 0.0, 1.0, 0.0,
    };
    ASSERT_TRUE (PlacePointCloudInProject (cloud, projectToSurvey, error)) << error;
    EXPECT_DOUBLE_EQ (cloud.origin[0], 83.25);
    EXPECT_DOUBLE_EQ (cloud.origin[1], -204.5);
    EXPECT_DOUBLE_EQ (cloud.origin[2], 120.0);
    EXPECT_FLOAT_EQ (cloud.vertices[1].position[0], 1.0f);
    EXPECT_FLOAT_EQ (cloud.vertices[1].position[1], 2.0f);
    EXPECT_FLOAT_EQ (cloud.vertices[1].position[2], 3.0f);
}

} // namespace
