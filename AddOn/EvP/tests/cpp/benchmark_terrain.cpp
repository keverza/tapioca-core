// Opt-in worker measurement. No production caller or timing assertions in CI.
#include "terrain_fixture.hpp"
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <new>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace {
thread_local bool s_count = false;
thread_local uint64_t s_calls = 0, s_bytes = 0;
} // namespace
// Ordinary C++ new only: no Rust/malloc/aligned allocation claim.
void* operator new (size_t bytes)
{
    void* pointer = std::malloc (bytes == 0 ? 1 : bytes);
    if (!pointer)
        throw std::bad_alloc ();
    if (s_count) {
        ++s_calls;
        s_bytes += bytes;
    }
    return pointer;
}
void* operator new[] (size_t bytes)
{
    return ::operator new (bytes);
}
void operator delete (void* pointer) noexcept
{
    std::free (pointer);
}
void operator delete[] (void* pointer) noexcept
{
    std::free (pointer);
}
void operator delete (void* pointer, size_t) noexcept
{
    std::free (pointer);
}
void operator delete[] (void* pointer, size_t) noexcept
{
    std::free (pointer);
}

namespace {
using namespace tapioca::terrain_fixture;
uint64_t PeakRss ()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters {};
    Require (GetProcessMemoryInfo (GetCurrentProcess (), &counters, sizeof (counters)) != 0, "memory counters");
    return counters.PeakWorkingSetSize;
#else
    rusage usage {};
    Require (getrusage (RUSAGE_SELF, &usage) == 0, "memory counters");
#ifdef __APPLE__
    return uint64_t (usage.ru_maxrss);
#else
    return uint64_t (usage.ru_maxrss) * 1024;
#endif
#endif
}
void Measure (const Fixture& input, size_t iterations)
{
    const auto constraints = input.Constraints ();
    const auto provenance = Provenance ({ input.points.size () / 3, input.Segments (), 0, 0 }, 3);
    const auto request = Request (input, constraints, provenance);
    const auto run = [&] {
        TerrainResult result;
        std::string error;
        Require (TriangulateTerrain (request, result, error), error);
        return result;
    };
    const auto expected = run ();
    CheckTin (input, expected);
    Require (expected.inputProvenance == provenance, "catalog retention");
    const auto hash = Digest (expected);
    for (size_t i = 0; i < 3; ++i)
        Require (Digest (run ()) == hash, "warmup repeatability");
    std::vector<double> samples;
    samples.reserve (iterations);
    const auto rssBefore = PeakRss ();
    for (size_t i = 0; i < iterations; ++i) {
        const auto start = std::chrono::steady_clock::now ();
        const auto result = run ();
        const double ms =
            std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - start).count ();
        Require (Digest (result) == hash, "measurement repeatability");
        samples.push_back (ms);
    }
    s_calls = s_bytes = 0;
    s_count = true;
    {
        const auto result = run ();
    }
    s_count = false;
    const auto calls = s_calls, bytes = s_bytes, rssAfter = PeakRss ();
    Require (calls > 0 && bytes > 0, "C++ allocation probe");
    std::sort (samples.begin (), samples.end ());
    // Mandatory successful fill copies, not measured memcpy traffic. Catalog,
    // descriptor and internal/reallocation copies are additional, not counted.
    const auto outputPayload = expected.points.size () * sizeof (double) +
                               expected.triangles.size () * sizeof (uint32_t) + expected.mappings.size () * 32;
    const auto mappingConversion = expected.mappings.size () * sizeof (TerrainMapping);
    std::cout << "{\"lane\":\"facade\",\"fixture\":\"" << input.name
              << "\",\"vertices\":" << expected.points.size () / 3
              << ",\"triangles\":" << expected.triangles.size () / 3 << ",\"constraints\":" << constraints.size ()
              << ",\"hash\":\"" << std::hex << std::setfill ('0') << std::setw (16) << hash << std::dec
              << "\",\"iterations\":" << iterations << ",\"median_ms\":" << samples.at (iterations / 2)
              << ",\"p95_ms\":" << samples.at ((iterations * 95 + 99) / 100 - 1) << ",\"cpp_new_calls\":" << calls
              << ",\"cpp_new_requested_bytes\":" << bytes << ",\"process_peak_rss_before_bytes\":" << rssBefore
              << ",\"process_peak_rss_after_bytes\":" << rssAfter
              << ",\"known_fill_payload_copy_bytes\":" << outputPayload
              << ",\"known_mapping_conversion_bytes\":" << mappingConversion
              << ",\"rust_alloc_calls\":null,\"copy_bytes\":null,\"combined_peak_heap_bytes\":null,\"configuration\":\""
              << TAPIOCA_BENCH_CONFIG
#ifdef _MSC_VER
              << "\",\"compiler\":\"MSVC\",\"compiler_version\":" << _MSC_FULL_VER
#elif defined(__clang__)
              << "\",\"compiler\":\"Clang\",\"compiler_version\":\"" << __clang_version__ << '"'
#else
              << "\",\"compiler\":\"GCC\",\"compiler_version\":\"" << __VERSION__ << '"'
#endif
              << "}\n";
    std::cout << "{\"samples_for\":\"" << input.name << "\",\"samples_ms\":[";
    for (size_t i = 0; i < samples.size (); ++i) {
        if (i)
            std::cout << ',';
        std::cout << samples[i];
    }
    std::cout << "]}\n";
}
} // namespace
int main (int argc, char** argv)
{
    try {
#ifndef NDEBUG
        throw std::runtime_error ("optimized measurement build required");
#endif
        s_calls = s_bytes = 0;
        s_count = true;
        void* probe = ::operator new (4096);
        ::operator delete (probe);
        s_count = false;
        Require (s_calls == 1 && s_bytes == 4096, "C++ new probe sanity");
        Require (argc == 3, "usage: TapiocaTerrainBench <fixture|grid_100x100_500> <iterations 3..1000>");
        size_t consumed = 0;
        const auto iterations = std::stoul (argv[2], &consumed);
        Require (consumed == std::string (argv[2]).size () && iterations >= 3 && iterations <= 1000,
                 "invalid iterations");
        if (std::string (argv[1]) == "grid_100x100_500") {
            Measure (Workload (), iterations);
            return 0;
        }
        for (const auto& input : Corpus ())
            if (input.name == argv[1]) {
                Measure (input, iterations);
                return 0;
            }
        throw std::runtime_error ("unknown fixture");
    }
    catch (const std::exception& error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}
