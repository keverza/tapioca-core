"""Hover must not depend on the whole-model BVH skipped by role partitioning."""

from pathlib import Path


def test_inspector_queries_only_the_gpu_picked_mesh_without_building_a_bvh():
    root = Path(__file__).parents[1] / "Sources/AddOn/ArchViz"
    source = (root / "DiligentHudSunStudy.cpp").read_text(encoding="utf-8")
    inspector = source.split("void ServiceSunStudyInspector", 1)[1].split("void DrawSunStudyInspectorTooltip", 1)[0]
    assert "QueryIndexCache::Get ()" not in inspector
    assert "RaycastAll" not in inspector
    assert "QueryEngine::RaycastMesh (*snapshot, mesh" in inspector
    assert "CanonicalGuid (snapshot->meshes[mesh].guid) == picked" in inspector
    assert "ReadAt (id, snapshot->id, hit.tri, hit.meshIndex, hit.point" in inspector
    assert "picked == lastPicked" in inspector
    assert "snapshot->id == lastSnapshot" in inspector
    assert "state.sunInspect == 0 || !state.hover.valid" in inspector
