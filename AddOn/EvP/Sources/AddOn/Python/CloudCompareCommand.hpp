#ifndef EVP_CLOUDCOMPARECOMMAND_HPP
#define EVP_CLOUDCOMPARECOMMAND_HPP

// Pure command-line construction for the pinned CloudCompare release. This
// header deliberately has no Archicad, GSRoot, or Win32 dependency so the
// contract can be tested by the offline C++ suite.

#include <string>
#include <vector>

namespace evp {

inline constexpr char kCloudComparePinnedVersion[] = "2.13.2";

struct CloudComparePoint {
    double x = 0.0;
    double y = 0.0;
};

struct CloudCompareCommandRequest {
    std::wstring executablePath;
    std::wstring logPath;
    std::wstring inputPath;
    std::wstring outputPath;
    std::vector<CloudComparePoint> cropPolygon;
    bool keepOutside = false;
    double subsampleStep = 0.0;
};

// Returns a mutable-command-line-compatible string for CreateProcessW.
// Every path is quoted and optional operations are omitted when disabled.
std::wstring BuildCloudCompareCommandLine (const CloudCompareCommandRequest& request);

} // namespace evp

#endif
