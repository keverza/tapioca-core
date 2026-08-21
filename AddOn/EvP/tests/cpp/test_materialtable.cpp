// ArchViz/MaterialTable — the model's surface pool, and the ordering rule that
// makes it usable.
//
// ⚠️ WHY OFFLINE: every failure this file guards against renders a complete,
// correct building in the WRONG COLOURS. From inside Archicad that is
// indistinguishable from a styling decision — nothing errors, nothing is
// missing, and the only way to notice is to already know what the model should
// look like. Same argument as MeshGroups, one layer up.
//
// It also pins the two conventions the producer has to get right and cannot
// verify at runtime: the pool index is 1-BASED, and alpha is the FLIP of
// ModelerAPI's transparency.

#include "ArchViz/MaterialTable.hpp"
#include "ArchViz/SceneCmdQueue.hpp"

#include <gtest/gtest.h>

#include <limits>

using namespace geomsrv::archviz;

namespace {

SurfaceMaterial Make (int32_t index, float r, float g, float b, float alpha = 1.0f)
{
    SurfaceMaterial m;
    m.index = index;
    m.r = r; m.g = g; m.b = b;
    m.alpha = alpha;
    return m;
}

}   // namespace

TEST (MaterialTable, EmptyTableAnswersEveryLookupWithOpaqueWhite)
{
    // THE FALLBACK IS LOAD-BEARING: geometry can reach the renderer before its
    // table does (a partial refresh, a dropped command), and a scene that
    // rendered black or invisible in that window would read as "the extraction
    // failed" rather than "the colours have not arrived".
    const MaterialTable table;
    ASSERT_TRUE (table.IsEmpty ());

    const SurfaceMaterial& m = table.Lookup (7);
    EXPECT_FLOAT_EQ (m.r, 1.0f);
    EXPECT_FLOAT_EQ (m.g, 1.0f);
    EXPECT_FLOAT_EQ (m.b, 1.0f);
    EXPECT_FLOAT_EQ (m.alpha, 1.0f);
    EXPECT_FALSE (table.Has (7));
}

TEST (MaterialTable, LooksUpByPoolIndexNotByPosition)
{
    // ⚠️ THE BUG THIS EXISTS FOR. `Mesh::triMaterial` carries the model pool's
    // index, which is 1-BASED and need not be contiguous. A table that indexed
    // its own vector by position would be off by one everywhere — every surface
    // painted in its neighbour's colour, which looks entirely plausible.
    MaterialTable table;
    table.Set (Make (1, 1.0f, 0.0f, 0.0f));
    table.Set (Make (2, 0.0f, 1.0f, 0.0f));
    table.Set (Make (9, 0.0f, 0.0f, 1.0f));

    EXPECT_FLOAT_EQ (table.Lookup (1).r, 1.0f);
    EXPECT_FLOAT_EQ (table.Lookup (2).g, 1.0f);
    EXPECT_FLOAT_EQ (table.Lookup (9).b, 1.0f);

    // Index 0 is ORDINARY — the extractor emits it for a polygon with no
    // material — so it must fall through to white rather than to entry #1.
    EXPECT_FALSE (table.Has (0));
    EXPECT_FLOAT_EQ (table.Lookup (0).r, 1.0f);
    EXPECT_FLOAT_EQ (table.Lookup (0).g, 1.0f);
}

TEST (MaterialTable, SetReplacesRatherThanAppendingADuplicate)
{
    MaterialTable table;
    table.Set (Make (3, 1.0f, 0.0f, 0.0f));
    table.Set (Make (3, 0.0f, 0.0f, 1.0f));

    EXPECT_EQ (table.Size (), 1u);
    EXPECT_FLOAT_EQ (table.Lookup (3).b, 1.0f);
    EXPECT_FLOAT_EQ (table.Lookup (3).r, 0.0f);
}

TEST (MaterialTable, OpaqueThresholdKeepsNearlySolidSurfacesOutOfTheBlendedPass)
{
    // A surface authored at "0% transparent" can arrive as a hair under 1.0.
    // Treating that as blended would put the WHOLE BUILDING in the transparent
    // pass, which does not write depth — the model then draws inside-out.
    EXPECT_FALSE (0.9999f < kOpaqueAlpha);
    EXPECT_FALSE (1.0f    < kOpaqueAlpha);
    // Real glass must still blend.
    EXPECT_TRUE (0.15f < kOpaqueAlpha);
    EXPECT_TRUE (0.9f  < kOpaqueAlpha);
}

TEST (MaterialTable, ShineIsReadAsAPhongExponentAndNotAsAGlossFraction)
{
    // ⚠️ THE REGRESSION THIS PINS IS INVISIBLE IN A STILL. The old mapping was
    // `1 - shininess`, which put the glossiest surface of the measured project
    // at roughness 0.718 -- and since the shader scales the sky reflection by
    // (1-roughness)^2, that is 7.9% of the reflection it should carry. Nothing
    // errors; the building simply renders with no reflections anywhere, which
    // is what was reported.
    //
    // The values below are the real pool, from SurfaceTemplateDump on
    // 2026-08-20 (23 surfaces used by the model). `shininess` is the DevKit's
    // shininess FACTOR divided by 100, so factor 28.18 arrives as 0.2818.
    SurfaceMaterial matte = Make (1, 1.0f, 1.0f, 1.0f);
    matte.shininess = 0.0f; // factor 0: brick, grass, plaster
    EXPECT_FLOAT_EQ (SurfaceRoughness (matte), 1.0f);

    SurfaceMaterial paint = Make (2, 1.0f, 1.0f, 1.0f);
    paint.shininess = 0.008f; // factor 0.8: RAL paint
    EXPECT_NEAR (SurfaceRoughness (paint), 0.845154f, 1e-5f);

    SurfaceMaterial steel = Make (3, 1.0f, 1.0f, 1.0f);
    steel.shininess = 0.18f; // factor 18: stainless
    EXPECT_NEAR (SurfaceRoughness (steel), 0.316228f, 1e-5f);

    SurfaceMaterial glossiest = Make (4, 1.0f, 1.0f, 1.0f);
    glossiest.shininess = 0.2818f; // factor 28.18: the pool's maximum
    EXPECT_NEAR (SurfaceRoughness (glossiest), 0.2574278f, 1e-5f);

    // The documented ceiling of the factor. Glossy, but deliberately not a
    // mirror -- Archicad has no channel that means "mirror".
    SurfaceMaterial maxed = Make (5, 1.0f, 1.0f, 1.0f);
    maxed.shininess = 1.0f; // factor 100
    EXPECT_NEAR (SurfaceRoughness (maxed), 0.140028f, 1e-5f);

    // Monotonic: more shine is never rougher.
    EXPECT_GT (SurfaceRoughness (matte), SurfaceRoughness (paint));
    EXPECT_GT (SurfaceRoughness (paint), SurfaceRoughness (steel));
    EXPECT_GT (SurfaceRoughness (steel), SurfaceRoughness (glossiest));
    EXPECT_GT (SurfaceRoughness (glossiest), SurfaceRoughness (maxed));
}

TEST (MaterialTable, OutOfSpecShineIsClampedRatherThanPropagated)
{
    // A factor outside [0..100] is out of spec, not a licence to return a
    // roughness outside [0..1]. Negative and NaN both have to land on matte.
    SurfaceMaterial negative = Make (1, 1.0f, 1.0f, 1.0f);
    negative.shininess = -5.0f;
    EXPECT_FLOAT_EQ (SurfaceRoughness (negative), 1.0f);

    SurfaceMaterial nan = Make (2, 1.0f, 1.0f, 1.0f);
    nan.shininess = std::numeric_limits<float>::quiet_NaN ();
    EXPECT_FLOAT_EQ (SurfaceRoughness (nan), 1.0f);

    SurfaceMaterial huge = Make (3, 1.0f, 1.0f, 1.0f);
    huge.shininess = 80.0f; // factor 8000, i.e. the RAW storage
    EXPECT_NEAR (SurfaceRoughness (huge), 0.140028f, 1e-5f);
}

TEST (MaterialTable, TransparentMaterialsKeepAVisibleGlassRoughness)
{
    // ⚠️ A FLOOR ON GLOSS, NOT A CLASSIFIER. Archicad supplies transparency,
    // shine and specular reflection but no IOR, refraction or metalness, so a
    // pane authored with no shine at all would otherwise come out fully matte
    // and reflect nothing -- glass that reads as flat tinted plastic.
    SurfaceMaterial unshinedGlass = Make (1, 0.4f, 0.6f, 0.9f, 0.5f);
    unshinedGlass.shininess = 0.0f;
    EXPECT_FLOAT_EQ (SurfaceRoughness (unshinedGlass), kTransparentRoughnessCeiling);

    // Authored shine that is ALREADY glossier than the floor is kept: the floor
    // must never make a well-authored pane rougher than the author said.
    SurfaceMaterial shinedGlass = Make (2, 0.4f, 0.6f, 0.9f, 0.5f);
    shinedGlass.shininess = 0.6f; // factor 60
    EXPECT_NEAR (SurfaceRoughness (shinedGlass), 0.179605f, 1e-5f);

    // The floor is keyed on alpha, and kOpaqueAlpha is the same threshold that
    // decides the blended pass -- so nothing can be floored here yet drawn as
    // opaque there.
    SurfaceMaterial nearlyOpaque = Make (3, 0.4f, 0.6f, 0.9f, kOpaqueAlpha);
    nearlyOpaque.shininess = 0.0f;
    EXPECT_FLOAT_EQ (SurfaceRoughness (nearlyOpaque), 1.0f);
}

TEST (MaterialTable, TravelsThroughTheQueueAsAnOwningHandover)
{
    // The consumer takes ownership of the table exactly as it does of an
    // ElementUpload — the producer must not keep a pointer into a buffer it
    // reuses (SceneCmdQueue.hpp's whole premise).
    SceneCmdQueue& q = SceneCmdQueue::Get ();
    q.Clear ();

    auto table = std::make_unique<MaterialTable> ();
    table->Set (Make (4, 0.25f, 0.5f, 0.75f, 0.4f));
    q.PushMaterials (std::move (table));
    EXPECT_EQ (table, nullptr);

    std::vector<SceneCmd> cmds = q.Take (4);
    ASSERT_EQ (cmds.size (), 1u);
    EXPECT_EQ (cmds[0].type, SceneCmdType::SetMaterials);
    ASSERT_NE (cmds[0].materials, nullptr);
    EXPECT_FLOAT_EQ (cmds[0].materials->Lookup (4).b, 0.75f);
    EXPECT_TRUE (cmds[0].materials->Lookup (4).alpha < kOpaqueAlpha);

    // And the byte accounting drops when it leaves, so a producer throttling on
    // PendingBytes can still see the queue drain.
    EXPECT_EQ (q.PendingCount (), 0u);
    EXPECT_EQ (q.PendingBytes (), 0u);
    q.Clear ();
}

TEST (MaterialTable, EnvironmentRidesTheQueueByValue)
{
    SceneCmdQueue& q = SceneCmdQueue::Get ();
    q.Clear ();

    EnvironmentUpload env;
    env.sunX = 0.3f;
    env.sunY = -0.4f;
    env.sunZ = 0.86f;
    env.ambient = 0.42f;
    env.sunBelowHorizon = true;
    q.PushEnvironment (env);

    std::vector<SceneCmd> cmds = q.Take (4);
    ASSERT_EQ (cmds.size (), 1u);
    EXPECT_EQ (cmds[0].type, SceneCmdType::SetEnvironment);
    EXPECT_FLOAT_EQ (cmds[0].environment.ambient, 0.42f);
    EXPECT_TRUE (cmds[0].environment.sunBelowHorizon);
    q.Clear ();
}

TEST (MaterialTable, BatchOrderPutsTheTableAheadOfItsGeometry)
{
    // ⚠️ THE ORDERING RULE, PINNED. The pool is RENUMBERED on every model
    // rebuild, so an upsert applied before its table is drawn against the
    // PREVIOUS pool. The producer pushes BeginBatch -> SetMaterials ->
    // SetEnvironment -> upserts, and Take must preserve exactly that.
    SceneCmdQueue& q = SceneCmdQueue::Get ();
    q.Clear ();

    q.PushBeginBatch (true);
    auto table = std::make_unique<MaterialTable> ();
    table->Set (Make (1, 0.1f, 0.2f, 0.3f));
    q.PushMaterials (std::move (table));
    q.PushEnvironment (EnvironmentUpload {});

    auto up = std::make_unique<ElementUpload> ();
    up->guid = "wall-1";
    up->vertices.assign (9, 0.0f);
    up->indices.assign (3, 0u);
    up->ranges.push_back (MaterialRange { 1, 0, 3 });
    q.PushUpsert (std::move (up));
    q.PushEndBatch ();

    std::vector<SceneCmd> cmds = q.Take (16);
    ASSERT_EQ (cmds.size (), 5u);
    EXPECT_EQ (cmds[0].type, SceneCmdType::BeginBatch);
    EXPECT_TRUE (cmds[0].full);
    EXPECT_EQ (cmds[1].type, SceneCmdType::SetMaterials);
    EXPECT_EQ (cmds[2].type, SceneCmdType::SetEnvironment);
    EXPECT_EQ (cmds[3].type, SceneCmdType::UpsertElement);
    EXPECT_EQ (cmds[4].type, SceneCmdType::EndBatch);
    q.Clear ();
}
