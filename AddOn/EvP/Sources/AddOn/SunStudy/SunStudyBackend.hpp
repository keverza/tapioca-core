#ifndef EVP_SUNSTUDY_SUNSTUDYBACKEND_HPP
#define EVP_SUNSTUDY_SUNSTUDYBACKEND_HPP

// Which traversal answers a sun study's rays, as asked and as it turned out.
//
// ⚠️ THE REQUEST IS AUTHORITATIVE AND STICKY. `StartSunStudy` falls back to a
// producer default only when the caller sends nothing; an explicit `cpu` is
// never upgraded because the producer is automatic (a follower replacement),
// and an explicit `gpu` is never rewritten to `cpu`. The replacement re-issues
// the adopted study's `StudyRecord::backend` verbatim, so the choice survives
// every edit.
//
// ⚠️ AND THE RESULT IS REPORTED, NEVER INFERRED. A GPU traversal may refuse at
// its lazy initialisation (no FP64 device, oversized scene, the developer's
// `TAPIOCA_SUNSTUDY_GPU=0`) and finish the study on the exact CPU tracer. The
// record still says `gpu` -- that is what was asked -- and the report says
// what ran and why, so the user can see it and rerun on CPU.

#include <string>

namespace evp::sunstudy {

class ITraversal;

inline constexpr const char* kCpuBackend = "cpu";
inline constexpr const char* kGpuBackend = "gpu";

bool IsSunStudyBackend (const std::string& name);

// The HUD's radio index (0 CPU, 1 GPU) as the `backend` parameter.
const char* SunStudyBackendForChoice (int choice);

// `StartSunStudy`'s `backend`: the caller's value when present, otherwise the
// producer's default (automatic studies may pick the GPU, manual ones are the
// CPU baseline). False with `error` for a value that is neither.
bool ResolveStartBackend (const std::string* requested, bool automatic, std::string& backend, std::string& error);

struct SunStudyBackendReport {
    std::string requested;
    // "cpu", "gpu", a worker's name, or empty while a lazily initialised
    // backend has not been tried yet.
    std::string resolved;
    std::string fallbackReason; // set when a requested GPU backend fell back
};

SunStudyBackendReport ReportSunStudyBackend (const std::string& requested, const ITraversal* traversal);

// One line for the HUD: "CPU", "GPU", "GPU unavailable, ran on CPU: <reason>",
// or "GPU requested, not engaged yet" -- a GPU traversal leaves batches below
// its minimum on the CPU, so a small study can finish without trying it.
std::string DescribeSunStudyBackend (const SunStudyBackendReport& report);

} // namespace evp::sunstudy

#endif
