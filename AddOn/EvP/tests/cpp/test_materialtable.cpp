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
    env.sunX = 0.3f; env.sunY = -0.4f; env.sunZ = 0.86f;
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
