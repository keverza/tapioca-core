// Opt-in measurement executable, never linked into the add-on or geometry tests.
#include "Geometry/GeometryRustTerrain.hpp"
#include "Geometry/GeometryRustFfi.h"
#include "Geometry/Transforms.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace {
// Counts only this thread's ordinary C++ new/new[], not Rust's allocator,
// malloc, aligned allocations, allocator overhead or other threads.
thread_local bool s_count = false;
thread_local uint64_t s_calls = 0, s_bytes = 0;
volatile double s_sink = 0;
} // namespace

void* operator new (size_t bytes)
{
    void* memory = std::malloc (bytes == 0 ? 1 : bytes);
    if (!memory)
        throw std::bad_alloc ();
    if (s_count) {
        ++s_calls;
        s_bytes += bytes;
    }
    return memory;
}
void* operator new[] (size_t bytes)
{
    return ::operator new (bytes);
}
void operator delete (void* memory) noexcept
{
    std::free (memory);
}
void operator delete[] (void* memory) noexcept
{
    std::free (memory);
}
void operator delete (void* memory, size_t) noexcept
{
    std::free (memory);
}
void operator delete[] (void* memory, size_t) noexcept
{
    std::free (memory);
}

namespace {
using geomsrv::engine::Transform;
constexpr uint32_t kSeed = 0x5EED2026u;
constexpr double kParityAbsolute = 1e-12;
struct Fixture {
    std::vector<Transform> cpp;
    std::vector<TapiocaAffineTransform> abi;
    uint64_t hash = 14695981039346656037ULL;
};
Fixture MakeFixture (size_t count)
{
    Fixture fixture;
    uint32_t state = kSeed;
    const auto number = [&] () {
        state = state * 1664525u + 1013904223u;
        return double (int (state % 2049u) - 1024) / 1048576.0;
    };
    for (size_t index = 0; index < count; ++index) {
        Transform matrix;
        TapiocaAffineTransform pod {};
        for (size_t row = 0; row < 3; ++row) {
            for (size_t column = 0; column < 4; ++column) {
                const double value = (row == column ? 1.0 : 0.0) + number ();
                matrix.m[row][column] = pod.m[row][column] = value;
                const auto bits = std::bit_cast<uint64_t> (value);
                for (unsigned byte = 0; byte < 8; ++byte) {
                    fixture.hash ^= (bits >> (byte * 8)) & 255u;
                    fixture.hash *= 1099511628211ULL;
                }
            }
        }
        fixture.cpp.push_back (matrix);
        fixture.abi.push_back (pod);
    }
    return fixture;
}
Transform Run (std::string_view lane, const Fixture& fixture)
{
    Transform result;
    if (lane == "cpp") {
        result = geomsrv::engine::Identity ();
        for (const auto& matrix : fixture.cpp)
            result = geomsrv::engine::Compose (matrix, result);
    }
    else if (lane == "abi") {
        TapiocaAffineTransform output {};
        std::array<char, 256> error {};
        const auto status = tapioca_compose_transforms (fixture.abi.data (), fixture.abi.size (), &output,
                                                        error.data (), error.size ());
        if (status != TAPIOCA_OK)
            throw std::runtime_error (error.data ());
        for (size_t row = 0; row < 3; ++row)
            for (size_t column = 0; column < 4; ++column)
                result.m[row][column] = output.m[row][column];
    }
    else {
        std::string error;
        if (!tapioca::geometry::ComposeTransforms (fixture.cpp, result, error))
            throw std::runtime_error (error);
    }
    return result;
}
void Check (const Transform& expected, const Transform& actual)
{
    for (size_t row = 0; row < 3; ++row)
        for (size_t column = 0; column < 4; ++column)
            if (!std::isfinite (actual.m[row][column]) ||
                std::abs (expected.m[row][column] - actual.m[row][column]) > kParityAbsolute)
                throw std::runtime_error ("composition parity failed");
}
uint64_t PeakRss ()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS memory {};
    if (!GetProcessMemoryInfo (GetCurrentProcess (), &memory, sizeof (memory)))
        throw std::runtime_error ("GetProcessMemoryInfo failed");
    return memory.PeakWorkingSetSize;
#else
    rusage usage {};
    if (getrusage (RUSAGE_SELF, &usage) != 0)
        throw std::runtime_error ("getrusage failed");
#ifdef __APPLE__
    return uint64_t (usage.ru_maxrss);
#else
    return uint64_t (usage.ru_maxrss) * 1024;
#endif
#endif
}
size_t PositiveBounded (std::string_view value, size_t maximum, bool zero = false)
{
    size_t number = 0;
    const auto parsed = std::from_chars (value.data (), value.data () + value.size (), number);
    if (parsed.ec != std::errc {} || parsed.ptr != value.data () + value.size () || number > maximum ||
        (!zero && number == 0))
        throw std::runtime_error ("invalid bounded benchmark argument");
    return number;
}
void Measure (std::string_view lane, size_t count, size_t iterations)
{
    const auto fixture = MakeFixture (count);
    // All correctness and allocation checks run before timing. In each fresh
    // process this necessarily contributes to whole-process peak RSS.
    const auto expected = Run ("cpp", fixture);
    for (auto path : { "cpp", "abi", "facade" })
        Check (expected, Run (path, fixture));
    for (size_t index = 0; index < 8; ++index)
        s_sink = Run (lane, fixture).m[0][3];
    const auto beforeRss = PeakRss ();
    s_calls = s_bytes = 0;
    s_count = true;
    const auto start = std::chrono::steady_clock::now ();
    for (size_t index = 0; index < iterations; ++index)
        s_sink = Run (lane, fixture).m[0][3];
    const auto finish = std::chrono::steady_clock::now ();
    s_count = false;
    const double nanoseconds = std::chrono::duration<double, std::nano> (finish - start).count ();
    const auto allocations = s_calls, allocatedBytes = s_bytes;
    const auto afterRss = PeakRss ();
    if (!std::isfinite (nanoseconds) || nanoseconds <= 0)
        throw std::runtime_error ("invalid elapsed time");
    Check (expected, Run (lane, fixture));
    // Sanity-check the ordinary-new probe against this known facade allocation.
    const uint64_t wantedCalls = lane == "facade" && count != 0 ? iterations : 0;
    // MSVC's large-vector allocation requests extra alignment/header bytes.
    // Observe those bytes rather than confusing logical payload with allocation.
    if (allocations != wantedCalls || allocatedBytes < wantedCalls * count * sizeof (TapiocaAffineTransform) ||
        (wantedCalls == 0 && allocatedBytes != 0))
        throw std::runtime_error ("C++ new probe mismatch: calls=" + std::to_string (allocations) + " expected=" +
                                  std::to_string (wantedCalls) + " requestedBytes=" + std::to_string (allocatedBytes));
#ifdef _MSC_VER
    constexpr auto compiler = "MSVC";
    constexpr auto compilerVersion = _MSC_FULL_VER;
#elif defined(__clang__)
    constexpr auto compiler = "Clang";
    constexpr auto compilerVersion = __clang_major__ * 10000 + __clang_minor__ * 100 + __clang_patchlevel__;
#else
    constexpr auto compiler = "GNU-compatible";
    constexpr auto compilerVersion = __GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__;
#endif
#ifdef NDEBUG
    constexpr auto optimized = "true";
#else
    constexpr auto optimized = "false";
#endif
    std::cout.precision (17);
    std::cout << "{\"lane\":\"" << lane << "\",\"count\":" << count << ",\"iterations\":" << iterations
              << ",\"seed\":" << kSeed << ",\"fixtureHash\":\"" << fixture.hash << "\",\"workerCount\":1"
              << ",\"compiler\":\"" << compiler << "\",\"compilerVersion\":" << compilerVersion
              << ",\"configuration\":\"" << TAPIOCA_BENCH_CONFIG << "\",\"optimized\":" << optimized
              << ",\"parityAbsoluteTolerance\":" << kParityAbsolute
              << ",\"nanosecondsPerCall\":" << nanoseconds / double (iterations) << ",\"cppNewCalls\":" << allocations
              << ",\"cppNewRequestedBytes\":" << allocatedBytes << ",\"peakRssBeforeBytes\":" << beforeRss
              << ",\"peakRssAfterBytes\":" << afterRss << ",\"checksum\":" << s_sink << "}\n";
}
} // namespace

int main (int argc, char** argv)
{
    try {
        if (argc == 2 && std::string_view (argv[1]) == "--self-test") {
            for (auto lane : { "cpp", "abi", "facade" })
                for (size_t count : { 0u, 1u, 16u, 256u, 4096u })
                    Measure (lane, count, 128);
            return 0;
        }
        if (argc != 4)
            throw std::runtime_error ("usage: <cpp|abi|facade> <chain-count 0..4096> <iterations 1..10000>");
        const std::string_view lane = argv[1];
        if (lane != "cpp" && lane != "abi" && lane != "facade")
            throw std::runtime_error ("unknown composition lane");
        Measure (lane, PositiveBounded (argv[2], 4096, true), PositiveBounded (argv[3], 10000));
        return 0;
    }
    catch (const std::exception& error) {
        s_count = false;
        std::cerr << error.what () << '\n';
        return 1;
    }
}
