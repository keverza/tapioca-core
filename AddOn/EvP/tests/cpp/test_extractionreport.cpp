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
    EXPECT_EQ (lists.gaps, "column x2 (invalid x2), none x2 (past the model's end, which holds 3361 now x2), "
                           "object x3 (no bodies x2, NURBS bodies only x1)");
}

TEST (ExtractionReport, AKindWithNoReasonRecordedIsTheCountAlone)
{
    const report::EmptyLists lists = report::DescribeEmpty ({ { "beam", 1 } }, Reasons {});
    EXPECT_EQ (lists.ordinary, "");
    EXPECT_EQ (lists.gaps, "beam x1");
}
