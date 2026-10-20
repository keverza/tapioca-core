#include "Geometry/GeometryRustPolygon.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace {
using namespace tapioca::geometry;
using geomsrv::engine::Vector3;

AttributedPolygon Fixture (std::vector<Vector3> points)
{
    AttributedPolygon value;
    value.polygon.points = std::move (points);
    auto& request = value.provenance;
    request.expectedSession = { 1 };
    const uint64_t count = value.polygon.points.size ();
    request.counts = { count, count, count == 0 ? 0U : 1U, 0 };
    request.domains = 7;
    auto& table = request.table;
    table.context = { 1, request.expectedSession, 0, 100 };
    table.sources = { { 0x20000000000001ULL, { 0, 0 } } };
    table.operations = { { 12, {} }, { 14, {} }, { 15, { 0, 2 } } };
    table.objects = { { 13, 15 } };
    table.sourceIds = { 0x20000000000001ULL };
    table.operationIds = { 14, 12, 15 }; // Ordered parents are deliberately not sorted.
    NativeBinding binding;
    binding.logicalKind = LogicalKind::Object;
    binding.logicalId = 13;
    const std::string guid = "9BB0ABD8-B1F3-4D78-855B-9106FD753BDA";
    std::copy (guid.begin (), guid.end (), binding.nativeGuid.begin ());
    binding.metadataPresent = true;
    binding.metadataUuid = { 9 };
    binding.role = { 0, 6 };
    table.bindings = { binding };
    table.roleBytes = { 'o', 'u', 't', 'p', 'u', 't' };
    table.supportTables = { { { 1, request.expectedSession, 0, 90 }, request.counts, true } };
    for (auto [domain, size] : { std::pair { IdentityDomain::Vertex, count },
                                 { IdentityDomain::Edge, count },
                                 { IdentityDomain::Face, request.counts.faces } }) {
        for (uint64_t index = 0; index < size; ++index) {
            EntityMapping row;
            row.domain = domain;
            row.kind = MappingKind::Exact;
            row.outputIndex = index;
            row.sources = { 0, 1 };
            row.operations = { 2, 1 };
            row.object = 13;
            row.predecessors = { table.predecessors.size (), 1 };
            table.predecessors.push_back ({ 90, domain, index });
            table.mappings.push_back (row);
        }
    }
    return value;
}
void Exact (const AttributedPolygon& actual, const AttributedPolygon& expected)
{
    EXPECT_EQ (actual.provenance, expected.provenance);
    ASSERT_EQ (actual.polygon.points.size (), expected.polygon.points.size ());
    for (size_t i = 0; i < actual.polygon.points.size (); ++i) {
        const auto a = actual.polygon.points[i], b = expected.polygon.points[i];
        EXPECT_EQ (std::bit_cast<uint64_t> (a.x), std::bit_cast<uint64_t> (b.x));
        EXPECT_EQ (std::bit_cast<uint64_t> (a.y), std::bit_cast<uint64_t> (b.y));
        EXPECT_EQ (std::bit_cast<uint64_t> (a.z), std::bit_cast<uint64_t> (b.z));
    }
}
void Roundtrip (const AttributedPolygon& source)
{
    const auto before = source;
    AttributedPolygon output;
    std::string error = "old";
    ASSERT_TRUE (CopyAttributedPolygon (source, output, error)) << error;
    EXPECT_TRUE (error.empty ());
    Exact (output, source);
    Exact (source, before);
    ASSERT_TRUE (CopyAttributedPolygon (output, output, error)) << error;
    Exact (output, source);
}
} // namespace

TEST (GeometryRustPolygon, ExactContourAndCompleteRetainedIdentityRoundtrip)
{
    // The saved analytic slab's independently read contour; this is value-copy
    // evidence only, not a live authored-ID/geometric ancestry assertion.
    Roundtrip (Fixture ({ { 100, 100, 0 }, { 120, 100, 0 }, { 120, 110, 0 }, { 100, 110, 0 } }));
    Roundtrip (Fixture ({ { -0.0, 6000000, 120 }, { 700002, 6000000, 120 }, { 700002, 6000003, 120 } }));
}

TEST (GeometryRustPolygon, WindingDuplicatesAndExplicitClosingVertexAreNotRepaired)
{
    std::vector<Vector3> points { { 0, 0, 0 }, { 4, 0, 0 }, { 4, 3, 0 }, { 0, 3, 0 } };
    Roundtrip (Fixture (points));
    std::reverse (points.begin (), points.end ());
    Roundtrip (Fixture (points));
    points.push_back (points.back ());
    points.push_back (points.front ());
    Roundtrip (Fixture (points));
}

TEST (GeometryRustPolygon, EmptyDegenerateSelfIntersectingAndNonplanarValuesDoNotClaimModellingValidity)
{
    Roundtrip (Fixture ({}));
    Roundtrip (Fixture ({ { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 } }));
    Roundtrip (Fixture ({ { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 } }));
    Roundtrip (Fixture ({ { 0, 0, 0 }, { 2, 2, 0 }, { 0, 2, 0 }, { 2, 0, 1 } }));
}

TEST (GeometryRustPolygon, SeededSurveyContoursPreserveEveryBitAndIdentity)
{
    uint32_t state = 0xC0A2026u;
    const auto random = [&] () {
        state = state * 1664525u + 1013904223u;
        return double (state) / 4294967296.0;
    };
    for (int i = 0; i < 64; ++i) {
        std::vector<Vector3> points;
        for (int j = 0; j < 3 + i % 13; ++j)
            points.push_back ({ 700000 + random () * 100, 6000000 + random () * 100, 120 + random () * 10 });
        Roundtrip (Fixture (std::move (points)));
    }
}

TEST (GeometryRustPolygon, NumericShapeMissingConflictingAndStaleAttributionNeverPublish)
{
    for (int kind = 0; kind < 11; ++kind) {
        auto source = Fixture ({ { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } });
        auto output = Fixture ({ { 77, 88, 99 }, { 77, 88, 99 }, { 77, 88, 99 } });
        const auto before = output;
        switch (kind) {
            case 0:
                source.polygon.points.resize (2);
                break;
            case 1:
                source.polygon.points.back ().z = std::numeric_limits<double>::quiet_NaN ();
                break;
            case 2:
                source.polygon.points.back ().z = std::numeric_limits<double>::infinity ();
                break;
            case 3:
                source.provenance.table.mappings.pop_back ();
                break;
            case 4:
                source.provenance.counts.edges = 0;
                break;
            case 5:
                source.provenance.domains = 5;
                break;
            case 6:
                source.provenance.expectedRevision = 1;
                break;
            case 7:
                source.provenance.table.objects.clear ();
                break;
            case 8:
                source.provenance.table.context.schemaVersion = 99;
                break;
            case 9:
                source.provenance.table.predecessors.back ().entityIndex = 999;
                break;
            default:
                source.provenance.table.bindings.back ().metadataUuid = {};
                break;
        }
        std::string error;
        EXPECT_FALSE (CopyAttributedPolygon (source, output, error)) << kind;
        EXPECT_FALSE (error.empty ());
        Exact (output, before);
    }
}

TEST (GeometryRustPolygon, SourceOnlyAttributionRequiresExplicitOptOutOfObjectOwnership)
{
    auto source = Fixture ({ { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } });
    source.provenance.table.objects.clear ();
    source.provenance.table.bindings.clear ();
    source.provenance.table.roleBytes.clear ();
    for (auto& row : source.provenance.table.mappings)
        row.object = 0;
    AttributedPolygon output;
    std::string error;
    EXPECT_FALSE (CopyAttributedPolygon (source, output, error));
    source.provenance.requireObject = false;
    Roundtrip (source);
}
