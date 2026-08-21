#ifndef EVP_CLOUDCOMPARERUNNER_HPP
#define EVP_CLOUDCOMPARERUNNER_HPP

// ACAPI-free CloudCompare process boundary. The caller must invoke this
// blocking function from an existing worker thread; it deliberately does not
// create a second thread or provide a route around MainThreadGate.

#include "UniString.hpp"

#include <cstddef>
#include <cstdint>

namespace evp {

struct CloudCompareResult {
    bool succeeded = false;
    bool cancelled = false;
    int exitCode = -1;
    GS::UniString transcript;
    GS::UniString outputPath;
    GS::UniString logPath;
    GS::UniString error;
};

// Runs the pinned CloudCompare 2.13.2 CLI as a child process. The polygon is a
// flat XY array with `cropPolygonCount` vertices. A zero subsample step omits
// subsampling. `runGeneration` is the RunCancel generation owned by the caller.
// The worker never calls ACAPI; any Archicad extraction needed to prepare these
// values must happen before this function, through MainThreadGate.
CloudCompareResult RunCloudCompareCli (const GS::UniString& executablePath, const GS::UniString& inputPath,
                                       const GS::UniString& outputPath, const double* cropPolygon,
                                       size_t cropPolygonCount, bool keepOutside, double subsampleStep,
                                       uint64_t runGeneration);

} // namespace evp

#endif
