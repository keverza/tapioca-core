#include "ArchViz/SunStudyGpuShader.hpp"
#include "ArchViz/SunStudyGpuTraversal.hpp"
#include "SunStudy/SunStudyOcclusion.hpp"
#include "MeshFixtures.hpp"

#include <gtest/gtest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <random>

using geomsrv::archviz::SunStudyGpuTraversal;
using Microsoft::WRL::ComPtr;

namespace {
std::shared_ptr<const geomsrv::Snapshot> MakeScene (double offset = 0.0, uint32_t roofs = 128)
{
    auto scene = std::make_shared<geomsrv::Snapshot> ();
    scene->id = 501;
    geomsrv::Mesh mesh;
    mesh.guid = "roof-comb";
    for (uint32_t i = 0; i < roofs; ++i) {
        const double x = offset + i * 2.0;
        const uint32_t base = static_cast<uint32_t> (mesh.vertices.size () / 3);
        const double quad[] = {
            x, offset, 5.0, x + 1.0, offset, 5.0, x + 1.0, offset + 1.0, 5.0, x, offset + 1.0, 5.0
        };
        mesh.vertices.insert (mesh.vertices.end (), std::begin (quad), std::end (quad));
        mesh.triangles.insert (mesh.triangles.end (), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
    scene->meshes.push_back (std::move (mesh));
    return scene;
}

std::vector<double> MakeOrigins (size_t count, double offset = 0.0)
{
    std::mt19937 random (511);
    std::uniform_real_distribution<double> x (-10.0, 266.0), y (-1.0, 2.0), z (-3.0, 8.0);
    std::vector<double> origins;
    for (size_t i = 0; i < count; ++i) {
        origins.push_back (offset + x (random));
        origins.push_back (offset + y (random));
        origins.push_back (z (random));
    }
    return origins;
}

bool Unavailable (const geomsrv::archviz::SunStudyGpuStats& stats)
{
    return !stats.available && (stats.error.find ("unavailable") != std::string::npos ||
                                stats.error.find ("disabled by") != std::string::npos);
}

geomsrv::Snapshot PartitionScene ()
{
    auto snapshot = *MakeScene ();
    snapshot.meshes[0].guid = "analysis";
    auto context = snapshot.meshes[0];
    context.guid = "site";
    for (size_t v = 0; v < context.vertices.size (); v += 3) {
        context.vertices[v] += 1.0;
        context.vertices[v + 2] += 2.0;
    }
    snapshot.meshes.push_back (std::move (context));
    return snapshot;
}

auto Partition (const geomsrv::Snapshot& scene, const evp::sunstudy::SunStudyOccluders* previous = nullptr)
{
    return evp::sunstudy::BuildSunStudyOccluders (scene, evp::sunstudy::ResolveElementRoles (scene, {}, { "site" }),
                                                  previous);
}
} // namespace

TEST (SunStudyGpuTraversal, ShaderCompilesAndMatchesPacketAbi)
{
    ComPtr<ID3DBlob> code, errors;
    const auto& shader = geomsrv::archviz::kSunStudyGpuShader;
    const HRESULT result = D3DCompile (
        shader, sizeof (shader) - 1, "SunStudyTest", nullptr, nullptr, "main", "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_IEEE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    ASSERT_TRUE (SUCCEEDED (result)) << (errors ? static_cast<const char*> (errors->GetBufferPointer ()) : "no errors");
    ComPtr<ID3D11ShaderReflection> reflection;
    ASSERT_TRUE (
        SUCCEEDED (D3DReflect (code->GetBufferPointer (), code->GetBufferSize (), IID_PPV_ARGS (&reflection))));
    UINT x = 0, y = 0, z = 0;
    EXPECT_EQ (reflection->GetThreadGroupSize (&x, &y, &z), 64u);
    EXPECT_EQ (x, 64u);
    EXPECT_EQ (y, 1u);
    EXPECT_EQ (z, 1u);
    auto* params = reflection->GetConstantBufferByName ("Parameters");
    D3D11_SHADER_BUFFER_DESC buffer {};
    ASSERT_TRUE (SUCCEEDED (params->GetDesc (&buffer)));
    EXPECT_EQ (buffer.Size, 144u);
    const std::pair<const char*, UINT> offsets[] = { { "inverseAndMin", 0 }, { "shearAndMax", 32 },
                                                     { "axesAndCount", 64 }, { "sceneAndFlags", 80 },
                                                     { "guard", 96 },        { "partitions", 128 } };
    for (const auto& [name, offset] : offsets) {
        D3D11_SHADER_VARIABLE_DESC desc {};
        ASSERT_TRUE (SUCCEEDED (params->GetVariableByName (name)->GetDesc (&desc))) << name;
        EXPECT_EQ (desc.StartOffset, offset) << name;
    }
    ComPtr<ID3DBlob> assembly;
    ASSERT_TRUE (SUCCEEDED (D3DDisassemble (code->GetBufferPointer (), code->GetBufferSize (), 0, nullptr, &assembly)));
    const std::string text (static_cast<const char*> (assembly->GetBufferPointer ()), assembly->GetBufferSize ());
    EXPECT_NE (text.find ("dcl_resource_structured t0, 64"), std::string::npos);
    EXPECT_NE (text.find ("dcl_resource_structured t1, 72"), std::string::npos);
    EXPECT_NE (text.find ("dcl_resource_structured t2, 24"), std::string::npos);
    EXPECT_NE (text.find ("dcl_resource_structured t3, 64"), std::string::npos);
    EXPECT_NE (text.find ("dcl_resource_structured t4, 72"), std::string::npos);
}

TEST (SunStudyGpuTraversal, EmptySceneExportsNoNodes)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    geomsrv::QueryEngine engine (snapshot);
    const auto exported = engine.ExportTraversalScene ();
    EXPECT_TRUE (exported.nodes.empty ());
    EXPECT_TRUE (exported.triangles.empty ());
}

TEST (SunStudyGpuTraversal, CancelledExportNeverReturnsPartialTree)
{
    geomsrv::QueryEngine engine (MakeScene (0.0, 4096));
    unsigned polls = 0;
    const auto scene = engine.ExportTraversalScene ([&] { return ++polls >= 3; });
    EXPECT_TRUE (scene.nodes.empty ());
    EXPECT_TRUE (scene.triangles.empty ());
    EXPECT_FALSE (engine.ExportTraversalScene ().nodes.empty ());
}

TEST (SunStudyGpuTraversal, ExportPreservesGeometryAndSubtrees)
{
    const auto snapshot = MakeScene ();
    geomsrv::QueryEngine engine (snapshot);
    const auto scene = engine.ExportTraversalScene ();
    ASSERT_GT (scene.nodes.size (), 1u);
    ASSERT_EQ (scene.triangles.size (), 256u);
    EXPECT_EQ (scene.nodes[0].escape, scene.nodes.size ());
    std::vector<std::array<double, 9>> actual, expected;
    size_t leafTriangles = 0;
    for (size_t i = 0; i < scene.nodes.size (); ++i) {
        const auto& node = scene.nodes[i];
        EXPECT_GT (node.escape, i);
        EXPECT_LE (node.escape, scene.nodes.size ());
        if (node.count == 0) {
            ASSERT_LT (i + 1, scene.nodes.size ());
            const auto right = scene.nodes[i + 1].escape;
            ASSERT_LT (right, node.escape);
            EXPECT_EQ (scene.nodes[right].escape, node.escape);
        }
        else {
            EXPECT_EQ (node.escape, i + 1);
            ASSERT_LE (static_cast<size_t> (node.first) + node.count, scene.triangles.size ());
            leafTriangles += node.count;
            for (size_t j = node.first; j < static_cast<size_t> (node.first) + node.count; ++j) {
                const auto& tri = scene.triangles[j];
                actual.push_back (
                    { tri.a[0], tri.a[1], tri.a[2], tri.b[0], tri.b[1], tri.b[2], tri.c[0], tri.c[1], tri.c[2] });
                for (const double* p : { tri.a, tri.b, tri.c }) {
                    for (int axis = 0; axis < 3; ++axis) {
                        EXPECT_GE (p[axis], node.min[axis]);
                        EXPECT_LE (p[axis], node.max[axis]);
                    }
                }
            }
        }
    }
    EXPECT_EQ (leafTriangles, scene.triangles.size ());
    const auto& mesh = snapshot->meshes[0];
    for (size_t triangle = 0; triangle < mesh.triangles.size (); triangle += 3) {
        std::array<double, 9> points {};
        for (size_t vertex = 0; vertex < 3; ++vertex)
            std::copy_n (&mesh.vertices[mesh.triangles[triangle + vertex] * 3], 3, &points[vertex * 3]);
        expected.push_back (points);
    }
    std::sort (actual.begin (), actual.end ());
    std::sort (expected.begin (), expected.end ());
    EXPECT_EQ (actual, expected);
}

TEST (SunStudyGpuTraversal, SmallBatchKeepsCpuAndDoesNotCreateDevice)
{
    auto engine = std::make_shared<geomsrv::QueryEngine> (MakeScene ());
    SunStudyGpuTraversal gpu (engine);
    const double positions[] = { 0.25, 0.5, 0.0, 1.25, 0.5, 0.0 };
    const double up[] = { 0.0, 0.0, 2.0 };
    uint8_t out[2] {};
    gpu.OccludeDirectional (positions, 2, up, 0.001, 0.0, out, 1);
    EXPECT_EQ (out[0], 1);
    EXPECT_EQ (out[1], 0);
    EXPECT_FALSE (gpu.Stats ().attempted);
    EXPECT_EQ (gpu.SceneVersion (), 501u);
}

TEST (SunStudyGpuTraversal, DisabledGpuFallsBackToCpuWithoutCreatingDevice)
{
    wchar_t previous[32] {};
    const DWORD size = GetEnvironmentVariableW (L"TAPIOCA_SUNSTUDY_GPU", previous, 32);
    ASSERT_LT (size, 32u);
    struct Restore {
        const wchar_t* previous;
        ~Restore ()
        {
            SetEnvironmentVariableW (L"TAPIOCA_SUNSTUDY_GPU", previous);
        }
    } restore { size == 0 ? nullptr : previous };
    ASSERT_TRUE (SetEnvironmentVariableW (L"TAPIOCA_SUNSTUDY_GPU", L"0"));
    auto engine = std::make_shared<geomsrv::QueryEngine> (MakeScene ());
    SunStudyGpuTraversal gpu (engine);
    evp::sunstudy::CpuTraversal cpu (engine);
    const auto origins = MakeOrigins (8197);
    std::vector<uint8_t> actual (8197), expected (8197);
    const double up[3] = { 0.0, 0.0, 1.0 };
    ASSERT_TRUE (gpu.OccludeDirectionalCancellable (origins.data (), actual.size (), up, 0.001, 0.0, actual.data (), 1,
                                                    [] { return false; }));
    cpu.OccludeDirectional (origins.data (), expected.size (), up, 0.001, 0.0, expected.data (), 1);
    EXPECT_EQ (actual, expected);
    EXPECT_TRUE (gpu.Stats ().attempted);
    EXPECT_FALSE (gpu.Stats ().available);
    EXPECT_EQ (gpu.Stats ().dispatches, 0u);
    EXPECT_EQ (gpu.Stats ().cpuFallbackRays, actual.size ());
    EXPECT_EQ (gpu.Stats ().error, "disabled by TAPIOCA_SUNSTUDY_GPU=0");
}

TEST (SunStudyGpuTraversal, HardwarePacketsMatchCpuAcrossDirectionsBoundsAndGeoreferencing)
{
    for (double offset : { 0.0, 1e9 }) {
        auto engine = std::make_shared<geomsrv::QueryEngine> (MakeScene (offset));
        SunStudyGpuTraversal gpu (engine);
        evp::sunstudy::CpuTraversal cpu (engine);
        const auto origins = MakeOrigins (8197, offset);
        std::vector<uint8_t> actual (8197), expected (8197);
        const double directions[][3] = {
            { 0.0, 0.0, 2.0 }, { 0.0, 0.0, -2.0 }, { 0.23, -0.11, 0.87 }, { 0.01, 1.0, 0.1 }, { 1.0, 0.0, 0.0 }
        };
        for (const auto& dir : directions) {
            for (double tmax : { 0.0, 3.0, 10.0 }) {
                cpu.OccludeDirectional (origins.data (), actual.size (), dir, 0.001, tmax, expected.data (), 1);
                gpu.OccludeDirectional (origins.data (), actual.size (), dir, 0.001, tmax, actual.data (), 1);
                if (Unavailable (gpu.Stats ()))
                    GTEST_SKIP () << gpu.Stats ().error;
                ASSERT_TRUE (gpu.Stats ().available) << gpu.Stats ().error;
                EXPECT_EQ (actual, expected) << "offset=" << offset << " tmax=" << tmax;
            }
        }
        EXPECT_EQ (gpu.Stats ().dispatches, 45u);
        EXPECT_EQ (gpu.Stats ().gpuRays, 8197u * 15);
        EXPECT_GT (gpu.Stats ().validationRays, 4096u);
        std::printf ("GPU sun parity: adapter=%s rays=%llu fallback=%llu\n", gpu.Stats ().adapter.c_str (),
                     static_cast<unsigned long long> (gpu.Stats ().gpuRays),
                     static_cast<unsigned long long> (gpu.Stats ().cpuFallbackRays));
    }
}

TEST (SunStudyGpuTraversal, BoundaryRaysAndAccumulatorBitsMatchCpu)
{
    auto engine = std::make_shared<geomsrv::QueryEngine> (MakeScene ());
    SunStudyGpuTraversal gpu (engine);
    evp::sunstudy::CpuTraversal cpu (engine);
    std::vector<double> origins;
    for (size_t i = 0; i < 8192; ++i) {
        const double x = static_cast<double> ((i / 8) % 128) * 2.0;
        const double positions[][3] = { { x, 0.0, 0.0 },
                                        { x + 0.5, 0.5, 0.0 },
                                        { x + 0.25, 0.5, 5.0 },
                                        { x + 1.0, 1.0, 0.0 },
                                        { x + 0.25, 0.5, 4.999 },
                                        { x + 1.25, 0.5, 0.0 },
                                        { x + 0.25, 0.5, 4.9990000001 },
                                        { x + 0.25, 0.5, 4.9989999999 } };
        origins.insert (origins.end (), std::begin (positions[i % 8]), std::end (positions[i % 8]));
    }
    evp::sunstudy::SampleSet samples;
    samples.positions = origins.data ();
    samples.count = origins.size () / 3;
    evp::sunstudy::OcclusionAccumulator actual (samples.count, 2), expected (samples.count, 2);
    const double up[3] = { 0.0, 0.0, 1.0 };
    ASSERT_TRUE (actual.AccumulateStep (gpu, samples, 0, up, 0.001, 0.0, 1));
    if (Unavailable (gpu.Stats ()))
        GTEST_SKIP () << gpu.Stats ().error;
    ASSERT_TRUE (gpu.Stats ().available) << gpu.Stats ().error;
    ASSERT_TRUE (expected.AccumulateStep (cpu, samples, 0, up, 0.001, 0.0, 1));
    ASSERT_TRUE (actual.AccumulateStep (gpu, samples, 1, up, 0.001, 5.0, 1));
    ASSERT_TRUE (expected.AccumulateStep (cpu, samples, 1, up, 0.001, 5.0, 1));
    EXPECT_EQ (actual.Bits (), expected.Bits ());
    EXPECT_GT (gpu.Stats ().cpuFallbackRays, 0u) << "edges and distance boundaries must use CPU resolution";
}

TEST (SunStudyGpuTraversal, ClosedAndSlopedSolidsAndThinOccludersMatchCpu)
{
    auto snapshot = std::make_shared<geomsrv::Snapshot> ();
    snapshot->id = 502;
    for (int i = 0; i < 64; ++i) {
        auto box = evptest::MakeBox (std::to_string (i), i * 4.0, -0.5, 0.0, 2.0, 1.5, i % 2 == 0 ? 0.001 : 5.0);
        for (size_t v = 0; v < box.vertices.size (); v += 3)
            box.vertices[v + 2] += box.vertices[v + 1] * 0.33;
        snapshot->meshes.push_back (std::move (box));
    }
    auto engine = std::make_shared<geomsrv::QueryEngine> (snapshot);
    SunStudyGpuTraversal gpu (engine);
    evp::sunstudy::CpuTraversal cpu (engine);
    const auto origins = MakeOrigins (16389);
    std::vector<uint8_t> actual (16389), expected (16389);
    const double directions[][3] = {
        { 0.17, 0.13, 0.71 }, { -0.99, 0.0, 0.01 }, { 0.0, -0.5, -0.7 }, { 0.0, 1.0, 0.33 }, { 0.0, 0.0, 1.0 }
    };
    for (const auto& direction : directions) {
        for (double tmin : { 0.0, 0.001, 0.2 }) {
            cpu.OccludeDirectional (origins.data (), actual.size (), direction, tmin, 0.0, expected.data (), 1);
            gpu.OccludeDirectional (origins.data (), actual.size (), direction, tmin, 0.0, actual.data (), 1);
            EXPECT_EQ (actual, expected);
            if (Unavailable (gpu.Stats ()))
                GTEST_SKIP () << gpu.Stats ().error;
            ASSERT_TRUE (gpu.Stats ().available) << gpu.Stats ().error;
        }
    }
}

TEST (SunStudyGpuTraversal, SelectiveTraversalPreservesReusableCompleteDayBits)
{
    auto engine = std::make_shared<geomsrv::QueryEngine> (MakeScene ());
    SunStudyGpuTraversal gpu (engine);
    evp::sunstudy::CpuTraversal cpu (engine);
    const auto origins = MakeOrigins (8192);
    evp::sunstudy::SampleSet samples;
    samples.positions = origins.data ();
    samples.count = origins.size () / 3;
    const double up[3] = { 0.0, 0.0, 1.0 }, down[3] = { 0.0, 0.0, -1.0 };
    evp::sunstudy::OcclusionAccumulator source (samples.count, 2), actual (samples.count, 2),
        expected (samples.count, 2);
    for (size_t step = 0; step < 2; ++step)
        ASSERT_TRUE (source.AccumulateStep (cpu, samples, step, down, 0.001, 0.0, 1));
    std::vector<size_t> mapping (samples.count, evp::sunstudy::OcclusionAccumulator::kNoReuse);
    for (size_t sample = 0; sample < samples.count; sample += 2)
        mapping[sample] = sample;
    ASSERT_TRUE (actual.SeedReusable (source, mapping));
    ASSERT_TRUE (expected.SeedReusable (source, mapping));
    for (size_t step = 0; step < 2; ++step) {
        ASSERT_TRUE (actual.AccumulateStep (gpu, samples, step, up, 0.001, 0.0, 1));
        ASSERT_TRUE (expected.AccumulateStep (cpu, samples, step, up, 0.001, 0.0, 1));
    }
    EXPECT_EQ (actual.Bits (), expected.Bits ());
    for (size_t sample = 0; sample < samples.count; sample += 2)
        for (size_t step = 0; step < 2; ++step)
            EXPECT_EQ (actual.Lit (sample, step), source.Lit (sample, step));
    if (Unavailable (gpu.Stats ()))
        GTEST_SKIP () << gpu.Stats ().error;
    ASSERT_TRUE (gpu.Stats ().available) << gpu.Stats ().error;
    EXPECT_EQ (gpu.Stats ().gpuRays, 4096u * 2);
}

TEST (SunStudyGpuTraversal, CancellationDoesNotCommitTimestep)
{
    auto engine = std::make_shared<geomsrv::QueryEngine> (MakeScene ());
    SunStudyGpuTraversal gpu (engine);
    const auto origins = MakeOrigins (65536);
    evp::sunstudy::SampleSet samples;
    samples.positions = origins.data ();
    samples.count = origins.size () / 3;
    evp::sunstudy::OcclusionAccumulator accumulator (samples.count, 1);
    const double up[3] = { 0.0, 0.0, 1.0 };
    std::vector<uint8_t> warmup (4096);
    gpu.OccludeDirectional (origins.data (), warmup.size (), up, 0.001, 0.0, warmup.data (), 1);
    const auto initial = gpu.Stats ();
    ASSERT_TRUE (initial.available || Unavailable (initial)) << initial.error;
    // Warm device creation separately. Cancel after compaction, either at a
    // packet wait or during result checks; partial output never resolves a step.
    unsigned polls = 0;
    ASSERT_FALSE (accumulator.AccumulateStep (gpu, samples, 0, up, 0.001, 0.0, 1, [&] { return ++polls > 20; }));
    EXPECT_FALSE (accumulator.StepResolved (0));
    EXPECT_EQ (accumulator.ResolvedStepCount (), 0u);
    EXPECT_TRUE (
        std::all_of (accumulator.Bits ().begin (), accumulator.Bits ().end (), [] (uint64_t bit) { return bit == 0; }));
    if (initial.available)
        EXPECT_GT (gpu.Stats ().dispatches, initial.dispatches);
    ASSERT_TRUE (accumulator.AccumulateStep (gpu, samples, 0, up, 0.001, 0.0, 1));
    evp::sunstudy::CpuTraversal cpu (engine);
    evp::sunstudy::OcclusionAccumulator expected (samples.count, 1);
    ASSERT_TRUE (expected.AccumulateStep (cpu, samples, 0, up, 0.001, 0.0, 1));
    EXPECT_EQ (accumulator.Bits (), expected.Bits ());
}

TEST (SunStudyGpuTraversal, ConcurrentDirectionalCallsShareNoScratch)
{
    auto engine = std::make_shared<geomsrv::QueryEngine> (MakeScene ());
    SunStudyGpuTraversal gpu (engine);
    evp::sunstudy::CpuTraversal cpu (engine);
    const auto origins = MakeOrigins (8192);
    const double up[3] = { 0.0, 0.0, 1.0 }, down[3] = { 0.0, 0.0, -1.0 };
    std::vector<uint8_t> a (8192), b (8192), expectedA (8192), expectedB (8192);
    auto first = std::async (
        std::launch::async, [&] { gpu.OccludeDirectional (origins.data (), a.size (), up, 0.001, 0.0, a.data (), 1); });
    gpu.OccludeDirectional (origins.data (), b.size (), down, 0.001, 0.0, b.data (), 1);
    first.get ();
    if (Unavailable (gpu.Stats ()))
        GTEST_SKIP () << gpu.Stats ().error;
    ASSERT_TRUE (gpu.Stats ().available) << gpu.Stats ().error;
    cpu.OccludeDirectional (origins.data (), a.size (), up, 0.001, 0.0, expectedA.data (), 1);
    cpu.OccludeDirectional (origins.data (), b.size (), down, 0.001, 0.0, expectedB.data (), 1);
    EXPECT_EQ (a, expectedA);
    EXPECT_EQ (b, expectedB);
}

TEST (SunStudyGpuTraversal, PartitionedSceneMatchesCombinedCpuAcrossDirectionsAndBounds)
{
    const auto scene = PartitionScene ();
    const auto parts = Partition (scene);
    SunStudyGpuTraversal gpu (parts->analysis, parts->context);
    evp::sunstudy::CpuTraversal cpu (
        std::make_shared<geomsrv::QueryEngine> (std::make_shared<geomsrv::Snapshot> (scene)));
    const auto origins = MakeOrigins (8197);
    std::vector<uint8_t> actual (8197), expected (8197);
    const double directions[][3] = { { 0, 0, 1 }, { 0, 0, -1 }, { 0.23, -0.11, 0.87 }, { 1, 0, 0 } };
    for (const auto& direction : directions) {
        for (double tmax : { 0.0, 3.0, 10.0 }) {
            gpu.OccludeDirectional (origins.data (), actual.size (), direction, 0.001, tmax, actual.data (), 1);
            cpu.OccludeDirectional (origins.data (), expected.size (), direction, 0.001, tmax, expected.data (), 1);
            EXPECT_EQ (actual, expected);
            if (Unavailable (gpu.Stats ()))
                GTEST_SKIP () << gpu.Stats ().error;
            ASSERT_TRUE (gpu.Stats ().available) << gpu.Stats ().error;
        }
    }
    EXPECT_GT (gpu.Stats ().contextUploadedBytes, 0u);
    EXPECT_GT (gpu.Stats ().sceneUploadedBytes, gpu.Stats ().contextUploadedBytes);
    EXPECT_FALSE (gpu.Stats ().contextReused);
    EXPECT_LE (gpu.Stats ().timedDispatches, gpu.Stats ().dispatches);
    EXPECT_GE (gpu.Stats ().computeMilliseconds, 0.0);
}

TEST (SunStudyGpuTraversal, EmptyAnalysisStillTracesContext)
{
    auto scene = PartitionScene ();
    scene.meshes.erase (scene.meshes.begin ());
    const auto parts = Partition (scene);
    SunStudyGpuTraversal gpu (parts->analysis, parts->context);
    evp::sunstudy::CpuTraversal cpu (parts->context);
    const auto origins = MakeOrigins (8192);
    const double up[] = { 0, 0, 1 };
    std::vector<uint8_t> actual (8192), expected (8192);
    gpu.OccludeDirectional (origins.data (), actual.size (), up, 0.001, 0, actual.data (), 1);
    cpu.OccludeDirectional (origins.data (), expected.size (), up, 0.001, 0, expected.data (), 1);
    EXPECT_EQ (actual, expected);
    if (Unavailable (gpu.Stats ()))
        GTEST_SKIP () << gpu.Stats ().error;
    ASSERT_TRUE (gpu.Stats ().available) << gpu.Stats ().error;
    EXPECT_EQ (gpu.Stats ().sceneUploadedBytes, gpu.Stats ().contextUploadedBytes);
}

TEST (SunStudyGpuTraversal, ReplacementsReuseContextAndDeviceAfterOriginalIsDestroyed)
{
    auto scene = PartitionScene ();
    const auto first = Partition (scene);
    auto original = std::make_unique<SunStudyGpuTraversal> (first->analysis, first->context);
    const auto origins = MakeOrigins (8192);
    const double up[] = { 0, 0, 1 };
    std::vector<uint8_t> actual (8192), expected (8192);
    original->OccludeDirectional (origins.data (), actual.size (), up, 0.001, 0, actual.data (), 1);
    if (Unavailable (original->Stats ()))
        GTEST_SKIP () << original->Stats ().error;
    ASSERT_TRUE (original->Stats ().available) << original->Stats ().error;
    scene.id = 502;
    for (size_t v = 0; v < scene.meshes[0].vertices.size (); v += 3)
        scene.meshes[0].vertices[v] += 0.25;
    const auto next = Partition (scene, first.get ());
    ASSERT_TRUE (next->contextReused);
    // A fully reused middle generation never dispatches. It must still carry
    // immutable resources to the next dirty generation, without owner pointers.
    auto middle = std::make_unique<SunStudyGpuTraversal> (next->analysis, next->context, original.get ());
    EXPECT_FALSE (middle->Stats ().attempted);
    original.reset ();
    SunStudyGpuTraversal replacement (next->analysis, next->context, middle.get ());
    middle.reset ();
    replacement.OccludeDirectional (origins.data (), actual.size (), up, 0.001, 0, actual.data (), 1);
    evp::sunstudy::CpuTraversal cpu (
        std::make_shared<geomsrv::QueryEngine> (std::make_shared<geomsrv::Snapshot> (scene)));
    cpu.OccludeDirectional (origins.data (), expected.size (), up, 0.001, 0, expected.data (), 1);
    EXPECT_EQ (actual, expected);
    ASSERT_TRUE (replacement.Stats ().available) << replacement.Stats ().error;
    EXPECT_TRUE (replacement.Stats ().deviceReused);
    EXPECT_TRUE (replacement.Stats ().contextReused);
    EXPECT_EQ (replacement.Stats ().contextUploadedBytes, 0u);
    EXPECT_GT (replacement.Stats ().sceneUploadedBytes, 0u);
    EXPECT_EQ (replacement.SceneVersion (), 502u);
}

TEST (SunStudyGpuTraversal, EditedContextUploadsFreshBuffersAndSharedDeviceCallsRemainIndependent)
{
    auto scene = PartitionScene ();
    const auto first = Partition (scene);
    SunStudyGpuTraversal original (first->analysis, first->context);
    const auto origins = MakeOrigins (8192);
    const double up[] = { 0, 0, 1 };
    std::vector<uint8_t> oldResult (8192), actual (8192), expected (8192), oldAgain (8192);
    original.OccludeDirectional (origins.data (), oldResult.size (), up, 0.001, 0, oldResult.data (), 1);
    if (Unavailable (original.Stats ()))
        GTEST_SKIP () << original.Stats ().error;
    ASSERT_TRUE (original.Stats ().available) << original.Stats ().error;
    scene.id = 502;
    for (size_t v = 0; v < scene.meshes[1].vertices.size (); v += 3)
        scene.meshes[1].vertices[v + 1] += 1000;
    const auto changed = Partition (scene, first.get ());
    ASSERT_FALSE (changed->contextReused);
    SunStudyGpuTraversal replacement (changed->analysis, changed->context, &original);
    auto concurrent = std::async (std::launch::async, [&] {
        original.OccludeDirectional (origins.data (), oldAgain.size (), up, 0.001, 0, oldAgain.data (), 1);
    });
    replacement.OccludeDirectional (origins.data (), actual.size (), up, 0.001, 0, actual.data (), 1);
    concurrent.get ();
    evp::sunstudy::CpuTraversal cpu (
        std::make_shared<geomsrv::QueryEngine> (std::make_shared<geomsrv::Snapshot> (scene)));
    cpu.OccludeDirectional (origins.data (), expected.size (), up, 0.001, 0, expected.data (), 1);
    EXPECT_EQ (actual, expected);
    EXPECT_EQ (oldAgain, oldResult);
    EXPECT_NE (oldResult, actual);
    ASSERT_TRUE (replacement.Stats ().available) << replacement.Stats ().error;
    EXPECT_TRUE (replacement.Stats ().deviceReused);
    EXPECT_FALSE (replacement.Stats ().contextReused);
    EXPECT_GT (replacement.Stats ().contextUploadedBytes, 0u);
}
