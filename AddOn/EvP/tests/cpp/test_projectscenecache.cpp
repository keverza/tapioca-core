// The viewer's per-project scene cache (ArchViz/ProjectSceneCache, #14): the file format and what it
// refuses, the surface-pool remap, the fingerprint rule, the reconciliation and the store.

#include "ArchViz/ProjectSceneCache.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <thread>

namespace sc = geomsrv::archviz::scenecache;
using geomsrv::CapturedMaterialIdentity;
using geomsrv::Mesh;
using geomsrv::Snapshot;
using geomsrv::archviz::MaterialTable;
using geomsrv::archviz::Substance;
using geomsrv::archviz::SurfaceMaterial;
using geomsrv::archviz::SurfaceSubstanceObservation;
using geomsrv::archviz::TexturePixels;

namespace {

Mesh MakeMesh (const std::string& guid, int32_t material)
{
    Mesh mesh;
    mesh.guid = guid;
    mesh.elemType = 3;
    mesh.vertices = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    mesh.normals = { 0, 0, 1, 0, 0, 1, 0, 0, 1 };
    mesh.triangles = { 0, 1, 2 };
    mesh.triMaterial = { material };
    mesh.uvs = { 0, 0, 1, 0, 0, 1 };
    mesh.triWireEdges = { 7 };
    mesh.triSourceBody = { 1 };
    mesh.triSourcePgon = { 1 };
    mesh.triSurfaceAttribute = { 12 };
    mesh.bounds.Expand (0, 0, 0);
    mesh.bounds.Expand (1, 1, 0);
    return mesh;
}

sc::State MakeState ()
{
    sc::State state;
    state.project = "solo:c:\\projects\\villa.pln";
    auto geometry = std::make_shared<Snapshot> ();
    geometry->captureStamp = 41;
    geometry->materialIdentity = { { 1, { "Brick", 10 } }, { 2, { "Glass", 20 } } };
    geometry->materialTransparency = { { 1, 0.0 }, { 2, 0.7 } };
    geometry->surfaceAttributes = { { 12, { "Brick", "{GUID}", 0xAA5533FFu } } };
    geometry->meshes = { MakeMesh ("A", 1), MakeMesh ("B", 2) };
    geometry->completeModel = true;
    state.geometry = geometry;
    state.records["A"] = { { 7, 0x1111, 0x2222 }, { { 1, Substance::Concrete, 6.0 } } };
    state.records["B"] = { { 8, 0x3333, 0 }, {} };
    auto materials = std::make_shared<MaterialTable> ();
    SurfaceMaterial brick;
    brick.index = 1;
    brick.r = 0.6f;
    brick.name = "Brick";
    brick.textureKey = 99;
    brick.substance = Substance::Concrete;
    materials->Set (brick);
    auto pixels = std::make_shared<TexturePixels> ();
    pixels->width = 2;
    pixels->height = 1;
    pixels->rgba = { 1, 2, 3, 4, 5, 6, 7, 8 };
    pixels->name = "brick.jpg";
    materials->SetTexture (99, pixels);
    state.materials = materials;
    state.haveEnvironment = true;
    state.environment.sunZ = 0.5f;
    state.environment.year = 2026;
    return state;
}

} // namespace

TEST (ProjectSceneCache, RoundTripKeepsEverything)
{
    const sc::State in = MakeState ();
    sc::State out;
    ASSERT_EQ (sc::Decode (sc::Encode (in), in.project, out), sc::DecodeStatus::Ok);
    EXPECT_EQ (out.project, in.project);
    ASSERT_EQ (out.geometry->meshes.size (), 2u);
    const Mesh& a = out.geometry->meshes[0];
    const Mesh& want = in.geometry->meshes[0];
    EXPECT_EQ (a.guid, "A");
    EXPECT_EQ (a.vertices, want.vertices);
    EXPECT_EQ (a.normals, want.normals);
    EXPECT_EQ (a.triangles, want.triangles);
    EXPECT_EQ (a.triMaterial, want.triMaterial);
    EXPECT_EQ (a.uvs, want.uvs);
    EXPECT_EQ (a.triWireEdges, want.triWireEdges);
    EXPECT_EQ (a.triSourceBody, want.triSourceBody);
    EXPECT_EQ (a.triSourcePgon, want.triSourcePgon);
    EXPECT_EQ (a.triSurfaceAttribute, want.triSurfaceAttribute);
    EXPECT_TRUE (a.bounds == want.bounds);
    EXPECT_EQ (out.geometry->materialIdentity, in.geometry->materialIdentity);
    EXPECT_EQ (out.geometry->materialTransparency, in.geometry->materialTransparency);
    EXPECT_EQ (out.geometry->surfaceAttributes, in.geometry->surfaceAttributes);
    EXPECT_EQ (out.geometry->captureStamp, 41u);
    EXPECT_TRUE (out.geometry->completeModel);
    EXPECT_EQ (out.records.at ("A").fingerprint, (sc::Fingerprint { 7, 0x1111, 0x2222 }));
    ASSERT_EQ (out.records.at ("A").observations.size (), 1u);
    EXPECT_EQ (out.records.at ("A").observations[0].substance, Substance::Concrete);
    EXPECT_DOUBLE_EQ (out.records.at ("A").observations[0].weight, 6.0);
    ASSERT_NE (out.materials, nullptr);
    EXPECT_EQ (out.materials->Lookup (1).name, "Brick");
    EXPECT_FLOAT_EQ (out.materials->Lookup (1).r, 0.6f);
    EXPECT_EQ (out.materials->Lookup (1).substance, Substance::Concrete);
    ASSERT_NE (out.materials->Texture (99), nullptr);
    EXPECT_EQ (out.materials->Texture (99)->rgba, (std::vector<uint8_t> { 1, 2, 3, 4, 5, 6, 7, 8 }));
    EXPECT_TRUE (out.haveEnvironment);
    EXPECT_TRUE (out.environment == in.environment);
}

TEST (ProjectSceneCache, RefusesAnotherVersion)
{
    std::string bytes = sc::Encode (MakeState ());
    bytes[8] = char (sc::kFormatVersion + 1);
    sc::State out;
    EXPECT_EQ (sc::Decode (bytes, "", out), sc::DecodeStatus::WrongVersion);
    EXPECT_EQ (out.geometry, nullptr);
}

TEST (ProjectSceneCache, RefusesCorruptAndTruncatedFiles)
{
    const std::string good = sc::Encode (MakeState ());
    sc::State out;
    std::string flipped = good;
    flipped[good.size () / 2] ^= 0x40;
    EXPECT_EQ (sc::Decode (flipped, "", out), sc::DecodeStatus::Corrupt);
    EXPECT_EQ (sc::Decode (good.substr (0, good.size () - 5), "", out), sc::DecodeStatus::Truncated);
    EXPECT_EQ (sc::Decode (good.substr (0, 12), "", out), sc::DecodeStatus::Truncated);
    EXPECT_EQ (sc::Decode (good + "x", "", out), sc::DecodeStatus::Corrupt);
    EXPECT_EQ (sc::Decode ("not a cache at all", "", out), sc::DecodeStatus::BadMagic);
    EXPECT_EQ (sc::Decode ("", "", out), sc::DecodeStatus::BadMagic);
    EXPECT_EQ (out.geometry, nullptr);
}

TEST (ProjectSceneCache, RefusesAnotherProject)
{
    sc::State out;
    EXPECT_EQ (sc::Decode (sc::Encode (MakeState ()), "solo:c:\\other.pln", out), sc::DecodeStatus::WrongProject);
}

TEST (ProjectSceneCache, RefusesAMeshIndexingPastItsVertices)
{
    sc::State state = MakeState ();
    auto geometry = std::make_shared<Snapshot> (*state.geometry);
    geometry->meshes[0].triangles = { 0, 1, 9 };
    state.geometry = geometry;
    sc::State out;
    EXPECT_EQ (sc::Decode (sc::Encode (state), "", out), sc::DecodeStatus::Corrupt);
}

TEST (ProjectSceneCache, PoolRemapIsIdentityWhenNothingRenumbered)
{
    const std::map<int32_t, CapturedMaterialIdentity> pool = { { 1, { "Brick", 10 } }, { 2, { "Glass", 20 } } };
    const sc::PoolRemap remap = sc::BuildPoolRemap (pool, pool);
    EXPECT_TRUE (remap.identity);
    EXPECT_TRUE (remap.unmapped.empty ());
}

TEST (ProjectSceneCache, PoolRemapFollowsIdentityNotIndex)
{
    const std::map<int32_t, CapturedMaterialIdentity> cached = { { 1, { "Brick", 10 } }, { 2, { "Glass", 20 } } };
    const std::map<int32_t, CapturedMaterialIdentity> live = { { 1, { "Glass", 20 } }, { 2, { "Concrete", 5 } },
                                                               { 3, { "Brick", 10 } } };
    const sc::PoolRemap remap = sc::BuildPoolRemap (cached, live);
    EXPECT_FALSE (remap.identity);
    EXPECT_EQ (remap.to.at (1), 3);
    EXPECT_EQ (remap.to.at (2), 1);

    Mesh mesh = MakeMesh ("A", 1);
    sc::ElementRecord record { {}, { { 2, Substance::Glass, 1.0 } } };
    ASSERT_TRUE (sc::CanRemap (mesh, record, remap));
    sc::Remap (mesh, record.observations, remap);
    EXPECT_EQ (mesh.triMaterial, (std::vector<int32_t> { 3 }));
    EXPECT_EQ (record.observations[0].surface, 1);
}

TEST (ProjectSceneCache, PoolRemapRefusesMissingAndAmbiguousIdentities)
{
    const std::map<int32_t, CapturedMaterialIdentity> cached = { { 1, { "Brick", 10 } }, { 2, { "Glass", 20 } } };
    const std::map<int32_t, CapturedMaterialIdentity> live = { { 1, { "Glass", 20 } }, { 2, { "Glass", 20 } } };
    const sc::PoolRemap remap = sc::BuildPoolRemap (cached, live);
    EXPECT_EQ (remap.unmapped, (std::set<int32_t> { 1, 2 }));
    EXPECT_FALSE (sc::CanRemap (MakeMesh ("A", 1), {}, remap));
    // An index the cached pool never held is not a pool reference: it passes.
    EXPECT_TRUE (sc::CanRemap (MakeMesh ("B", 0), {}, remap));
    // An observation on an unmapped surface also refuses the element.
    EXPECT_FALSE (sc::CanRemap (MakeMesh ("C", 0), { {}, { { 2, Substance::Glass, 1.0 } } }, remap));
}

TEST (ProjectSceneCache, MatchRuleNeedsOneTierToAgreeInFull)
{
    const sc::Fingerprint cached { 7, 0xAB, 0xCD };
    EXPECT_EQ (sc::MatchCheap (cached, 7, 0xAB), sc::Match::Cheap);
    EXPECT_EQ (sc::MatchCheap (cached, 8, 0xAB), sc::Match::NeedDeep);
    EXPECT_EQ (sc::MatchCheap (cached, 7, 0xAC), sc::Match::None); // counts moved: never reuse
    EXPECT_EQ (sc::MatchDeep (cached, 0xCD), sc::Match::Deep);
    EXPECT_EQ (sc::MatchDeep (cached, 0xCE), sc::Match::None);
    EXPECT_EQ (sc::MatchDeep (cached, 0), sc::Match::None); // an unread deep tier proves nothing
    // A cached element whose deep tier was never read cannot be rescued by one.
    EXPECT_EQ (sc::MatchCheap ({ 7, 0xAB, 0 }, 8, 0xAB), sc::Match::None);
}

TEST (ProjectSceneCache, ReconcileSortsUnchangedChangedDeletedAndNew)
{
    const std::set<std::string> cached = { "same", "edited", "deleted" };
    const sc::Reconciliation r = sc::Reconcile (cached, { "same" }, { "edited", "new" });
    EXPECT_EQ (r.reused, (std::vector<std::string> { "same" }));
    EXPECT_EQ (r.changed, (std::vector<std::string> { "edited" }));
    EXPECT_EQ (r.appeared, (std::vector<std::string> { "new" }));
    EXPECT_EQ (r.stale, (std::vector<std::string> { "deleted" }));
    EXPECT_TRUE (r.Any ());
    EXPECT_FALSE (sc::Reconcile ({ "a", "b" }, { "a", "b" }, {}).Any ());
}

TEST (ProjectSceneCache, StoreWritesReadsAndEvictsOldestFirst)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path () / "tapioca-scenecache-test";
    std::error_code ec;
    fs::remove_all (dir, ec);
    const fs::path a = sc::FileFor (dir, "solo:a");
    const fs::path b = sc::FileFor (dir, "solo:b");
    const fs::path c = sc::FileFor (dir, "solo:c");
    EXPECT_NE (a, b);
    EXPECT_EQ (a.extension (), ".tsc");
    std::string error;
    const std::string bytes (1000, 'x');
    ASSERT_TRUE (sc::WriteAtomic (a, bytes, error)) << error;
    fs::last_write_time (a, fs::file_time_type::clock::now () - std::chrono::hours (2));
    ASSERT_TRUE (sc::WriteAtomic (b, bytes, error)) << error;
    fs::last_write_time (b, fs::file_time_type::clock::now () - std::chrono::hours (1));
    ASSERT_TRUE (sc::WriteAtomic (c, bytes, error)) << error;
    std::string back;
    ASSERT_TRUE (sc::ReadWhole (c, back));
    EXPECT_EQ (back, bytes);
    // Over budget by two files; `a` is oldest, then `b`, and `c` is kept.
    EXPECT_EQ (sc::Evict (dir, 1500, c), 2u);
    EXPECT_FALSE (fs::exists (a));
    EXPECT_FALSE (fs::exists (b));
    EXPECT_TRUE (fs::exists (c));
    // Rewriting replaces in place; no temp file is left behind.
    ASSERT_TRUE (sc::WriteAtomic (c, "new", error)) << error;
    ASSERT_TRUE (sc::ReadWhole (c, back));
    EXPECT_EQ (back, "new");
    EXPECT_FALSE (fs::exists (fs::path (c.string () + ".tmp")));
    fs::remove_all (dir, ec);
}
