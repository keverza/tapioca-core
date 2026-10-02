// ArchViz/ExtractionReport: the two lines an extraction pass's empty elements are told in.
// Ordinary kinds (2D, a composite's parts) stay a count; a gap -- a solid element the overlay
// is not drawing -- says why, the commonest reason first, so one live run can choose a fix.

#include "ArchViz/ExtractionReport.hpp"

#include <gtest/gtest.h>

#include <map>
#include <string>

namespace report = geomsrv::archviz::extractionreport;

using Reasons = std::map<std::string, std::map<std::string, uint32_t>>;

TEST (ExtractionReport, AGapSaysWhyTheCommonestReasonFirst)
{
    const std::map<std::string, uint32_t> byType {
        { "object", 3 }, { "column", 2 }, { "+railing node", 4 }, { "dimension", 1 }, { "none", 2 }
    };
    const Reasons reasons {
        { "object", { { "NURBS bodies only", 1 }, { "no bodies", 2 } } },
        { "column", { { "invalid", 2 } } },
        { "+railing node", { { "no bodies", 4 } } },
        { "dimension", { { "no bodies", 1 } } },
        { "none", { { "past the model's end, which holds 3361 now", 2 } } },
    };
    const report::EmptyLists lists = report::DescribeEmpty (byType, reasons);
    EXPECT_EQ (lists.ordinary, "railing node x4, dimension x1");
    EXPECT_EQ (lists.carried, "") << "an invalid column is carried by nothing";
    EXPECT_EQ (lists.gaps, "column x2 (invalid x2), none x2 (past the model's end, which holds 3361 now x2), "
                           "object x3 (no bodies x2, NURBS bodies only x1)");
}

// 2026-10-02: every window, door, column, beam, stair, railing and the curtain wall of a 3889-element
// project read as gaps while the overlay drew them all -- an opening's geometry is its host's, a
// container's is on its parts.
TEST (ExtractionReport, AnElementWhoseGeometryIsAnothersIsCarriedNotAGap)
{
    const std::map<std::string, uint32_t> byType { { "window", 229 }, { "door", 12 },       { "column", 44 },
                                                   { "railing", 54 }, { "curtainwall", 1 }, { "object", 3 } };
    const std::string empty = "tessellated bodies: no vertices; mesh bodies beside, no polygons";
    const Reasons reasons {
        { "window", { { empty, 229 } } }, { "door", { { empty, 12 } } },       { "column", { { empty, 44 } } },
        { "railing", { { empty, 54 } } }, { "curtainwall", { { empty, 1 } } }, { "object", { { "no bodies", 3 } } },
    };
    const report::EmptyLists lists = report::DescribeEmpty (byType, reasons);
    EXPECT_EQ (lists.carried, "column x44, curtainwall x1, door x12, railing x54, window x229");
    EXPECT_EQ (lists.gaps, "object x3 (no bodies x3)");
}

TEST (ExtractionReport, ACarriedKindTheModelChangedAwayFromIsStillAGap)
{
    // Never read at all: the model changed under the pass (`past the model's end`).
    const Reasons reasons {
        { "window", { { "tessellated bodies: no vertices", 10 }, { "past the model's end, which holds 7 now", 5 } } }
    };
    const report::EmptyLists lists = report::DescribeEmpty ({ { "window", 15 } }, reasons);
    EXPECT_EQ (lists.carried, "window x10");
    EXPECT_EQ (lists.gaps, "window x5 (past the model's end, which holds 7 now x5)");
}

TEST (ExtractionReport, AKindWithNoReasonRecordedIsTheCountAlone)
{
    const report::EmptyLists lists = report::DescribeEmpty ({ { "wall", 1 } }, Reasons {});
    EXPECT_EQ (lists.ordinary, "");
    EXPECT_EQ (lists.gaps, "wall x1");
}
