#include "SunStudy/SunStudyLimits.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace evp::sunstudy {

namespace {

std::mutex gMutex;
MachineResources gGpu;
std::vector<LimitReportLine> gReport;

std::string Format (const char* format, ...)
{
    char text[256];
    va_list args;
    va_start (args, format);
    std::vsnprintf (text, sizeof (text), format, args);
    va_end (args);
    return text;
}

// Millions, as the readout prints a count; "?" for a ceiling never measured.
std::string Millions (size_t count, bool known = true)
{
    return known ? Format ("%.1f M", double (count) / 1.0e6) : std::string ("?");
}

std::string ShownBackend (const LimitReportInputs& in)
{
    if (in.shownRequestedBackend.empty ())
        return "backend not recorded here (started outside this panel)";
    if (in.shownResolvedBackend.empty ())
        return "asked for " + in.shownRequestedBackend + ", not traced yet";
    std::string text = "asked for " + in.shownRequestedBackend + ", ran on " + in.shownResolvedBackend;
    if (!in.shownFallbackReason.empty ())
        text += ": " + in.shownFallbackReason;
    return text;
}

} // namespace

const char* LimitBindingName (LimitBinding binding)
{
    switch (binding) {
        case LimitBinding::SystemMemory:
            return "system memory";
        case LimitBinding::GpuMemory:
            return "display atlas GPU memory";
        case LimitBinding::TextureSize:
            return "display texture size";
        default:
            return "the sampler's cap";
    }
}

std::vector<LimitReportLine> DescribeAnalysisLimits (const AnalysisLimits& limits, const MachineResources& machine,
                                                     const LimitReportInputs& in)
{
    using Tone = LimitReportTone;
    std::vector<LimitReportLine> out;
    const auto add = [&] (Tone tone, std::string text) { out.push_back ({ tone, std::move (text) }); };

    add (Tone::Heading,
         Format ("Next run: %s, patch domain, up to %u steps", in.gpuSelected ? "GPU" : "CPU", unsigned (in.steps)));
    add (Tone::Plain, Format ("max %.2f M samples, set by %s", double (limits.maxSamples) / 1.0e6,
                              LimitBindingName (limits.binding)));
    const bool ramKnown = machine.availableRamBytes > 0;
    const bool vramKnown = machine.gpuKnown && machine.gpuMemoryBytes > 0;
    add (Tone::Quiet, "sampler " + Millions (limits.samplerCap) + " | free memory " +
                          Millions (limits.ramCap, ramKnown) + " | display atlas VRAM " +
                          Millions (limits.gpuCap, vramKnown) + " | display texture " + Millions (limits.textureCap));
    if (machine.gpuKnown)
        add (Tone::Quiet, Format ("display GPU %s, %.1f GB, textures up to %u", machine.adapter.c_str (),
                                  double (machine.gpuMemoryBytes) / 1.0e9, machine.maxTextureDimension));
    else
        add (Tone::Quiet, "display GPU not known yet: only the 8192 packer texture limit applies");
    if (ramKnown)
        add (Tone::Quiet, Format ("free memory %.1f GB now - an estimate; the run re-reads it when it starts",
                                  double (machine.availableRamBytes) / 1.0e9));
    else
        add (Tone::Quiet, "free memory not known: it does not bind");
    // What the backend consumes, so neither reads the display rows as compute.
    add (Tone::Quiet, in.gpuSelected ? "GPU tracing checks its own scene budget when it starts; the display rows "
                                       "above are the result atlas every backend uploads"
                                     : "CPU tracing uses system memory; the display rows above are the result "
                                       "atlas every backend uploads, not compute");
    add (Tone::Quiet,
         Format ("~%.0f M rays at that ceiling (an estimate, not a limit)", double (limits.maxRays) / 1.0e6));
    if (in.analysedArea > 0.0 && in.finestGridSpacing > 0.0)
        add (Tone::Plain,
             Format ("finest grid: %.2f m over the shown study's %.0f m2", in.finestGridSpacing, in.analysedArea));

    if (in.shownStudyId.empty ()) {
        add (Tone::Heading, "Shown study: none");
        return out;
    }
    add (Tone::Heading, Format ("Shown study: %s, %s domain, %u steps%s", in.shownStudyId.c_str (),
                                in.shownPatchDomain ? "patch" : "triangle", unsigned (in.shownSteps),
                                in.shownPreview ? ", coarse preview" : ""));
    add (Tone::Quiet, ShownBackend (in));
    if (limits.maxSamples > 0) {
        const double used = double (in.shownSamples) / double (limits.maxSamples);
        add (used > 0.8 ? Tone::Warning : Tone::Plain,
             Format ("%.2f M samples, %.0f%% of the next run's limit", double (in.shownSamples) / 1.0e6, used * 100.0));
    }
    else
        add (Tone::Warning, Format ("%.2f M samples; the next run's limit is 0", double (in.shownSamples) / 1.0e6));
    return out;
}

void PublishLimitReport (std::vector<LimitReportLine> lines)
{
    std::lock_guard<std::mutex> lock (gMutex);
    gReport = std::move (lines);
}

std::vector<LimitReportLine> PublishedLimitReport ()
{
    std::lock_guard<std::mutex> lock (gMutex);
    return gReport;
}

void PublishGpuResources (const std::string& adapter, uint64_t gpuMemoryBytes, uint32_t maxTextureDimension)
{
    std::lock_guard<std::mutex> lock (gMutex);
    gGpu.gpuKnown = true;
    gGpu.adapter = adapter;
    gGpu.gpuMemoryBytes = gpuMemoryBytes;
    gGpu.maxTextureDimension = maxTextureDimension;
}

std::string AtlasDisplayRefusal (uint32_t width, uint32_t height, uint32_t deviceMaxDimension)
{
    if (deviceMaxDimension == 0 || (width <= deviceMaxDimension && height <= deviceMaxDimension))
        return std::string ();
    return "the " + std::to_string (width) + "x" + std::to_string (height) + " atlas exceeds this GPU's " +
           std::to_string (deviceMaxDimension) + "x" + std::to_string (deviceMaxDimension) +
           " texture limit - run the study again so it is packed for this GPU, or ask for a coarser grid";
}

uint64_t AvailableSystemMemory ()
{
#ifdef _WIN32
    MEMORYSTATUSEX memory = {};
    memory.dwLength = sizeof (memory);
    return ::GlobalMemoryStatusEx (&memory) ? uint64_t (memory.ullAvailPhys) : 0u;
#else
    return 0u;
#endif
}

MachineResources CurrentMachineResources (uint64_t availableRamBytes)
{
    std::lock_guard<std::mutex> lock (gMutex);
    MachineResources out = gGpu;
    out.availableRamBytes = availableRamBytes;
    return out;
}

} // namespace evp::sunstudy
