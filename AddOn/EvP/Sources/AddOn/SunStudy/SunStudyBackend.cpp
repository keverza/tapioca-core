#include "SunStudy/SunStudyBackend.hpp"

#include "SunStudy/ITraversal.hpp"

namespace evp::sunstudy {

bool IsSunStudyBackend (const std::string& name)
{
    return name == kCpuBackend || name == kGpuBackend;
}

const char* SunStudyBackendForChoice (int choice)
{
    return choice == 1 ? kGpuBackend : kCpuBackend;
}

bool ResolveStartBackend (const std::string* requested, bool automatic, std::string& backend, std::string& error)
{
    backend = requested != nullptr ? *requested : std::string (automatic ? kGpuBackend : kCpuBackend);
    if (IsSunStudyBackend (backend))
        return true;
    error = "backend must be cpu or gpu";
    return false;
}

SunStudyBackendReport ReportSunStudyBackend (const std::string& requested, const ITraversal* traversal)
{
    SunStudyBackendReport report;
    report.requested = requested;
    if (traversal == nullptr) {
        report.resolved = kCpuBackend;
        return report;
    }
    const TraversalBackendState state = traversal->ResolvedBackend ();
    report.resolved = state.resolved;
    report.fallbackReason = state.fallbackReason;
    return report;
}

std::string DescribeSunStudyBackend (const SunStudyBackendReport& report)
{
    if (!report.fallbackReason.empty ())
        return "GPU unavailable, ran on CPU: " + report.fallbackReason;
    if (report.resolved.empty ())
        return "GPU requested, not engaged yet";
    if (report.resolved == kCpuBackend)
        return "CPU";
    if (report.resolved == kGpuBackend)
        return "GPU";
    return report.resolved; // a ray worker: "dxr", "optix"
}

} // namespace evp::sunstudy
