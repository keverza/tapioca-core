// SunStudy/SunStudyRecordCore -- attaching the Rust study core to a record and
// the calls that must ask whichever core runs it (P5). Compiled in every
// build; the Rust half exists only with the archive (TAPIOCA_GEOMETRY_RUST).

#include "SunStudy/SunStudyStore.hpp"

#if defined(TAPIOCA_GEOMETRY_RUST)
#include "SunStudy/SunStudyRustStudy.hpp"
#endif

namespace evp::sunstudy {

StudyProgress StudyRecord::CurrentProgress () const
{
#if defined(TAPIOCA_GEOMETRY_RUST)
    if (rustCore != nullptr) {
        StudyProgress progress;
        std::string error;
        rustCore->Progress (progress, error); // a poisoned core reports an empty study
        return progress;
    }
#endif
    return session.Progress ();
}

void SyncStudy (StudyRecord& record, const StudyInputs& inputs)
{
#if defined(TAPIOCA_GEOMETRY_RUST)
    if (record.rustCore != nullptr) {
        std::string error;
        record.rustCore->Sync (inputs, error);
        return;
    }
#endif
    record.session.Sync (inputs, record.series, record.Samples ());
}

bool AttachRustStudyCore (StudyRecord& record, const geomsrv::Snapshot& snapshot, const StudyRecord* previous,
                          uint32_t maxAtlasDimension, std::string& error)
{
#if defined(TAPIOCA_GEOMETRY_RUST)
    const rust::RustStudyCore* earlier = previous != nullptr ? previous->rustCore.get () : nullptr;
    std::unique_ptr<rust::RustStudyCore> core;
    uint32_t width = 0, height = 0;
    size_t faces = 0;
    bool hasAtlas = true;
    if (record.IsPatchDomain ()) {
        core = rust::RustStudyCore::Patch (record.series, record.gridSpacing, record.patchGrid, earlier,
                                           maxAtlasDimension, error);
        width = record.patchAtlas.Width ();
        height = record.patchAtlas.Height ();
        faces = record.patchGrid.patchOfTriangle.size ();
    }
    else if (record.sampleGrid.Count () != 0) {
        rust::RustStudyCore::TriangleInputs inputs;
        inputs.grid = &record.sampleGrid;
        inputs.snapshot = &snapshot;
        core = rust::RustStudyCore::Triangle (record.series, record.gridSpacing, inputs, earlier, error);
        width = record.atlas.width;
        height = record.atlas.height;
        faces = record.atlas.tiles.size ();
        hasAtlas = record.atlas.valid;
    }
    else {
        core = rust::RustStudyCore::Ground (record.series, record.gridSpacing, record.positions, record.normals, error);
        hasAtlas = false;
    }
    if (core == nullptr)
        return false;

    // ⚠️ THE TWO HALVES MUST DESCRIBE ONE STUDY. The display assembler draws
    // through the record's atlas and the core scatters into its own; parity
    // makes them equal, and a disagreement here would paint every face with
    // another face's sunlight. Refuse rather than draw it.
    TapiocaStudyInfo info {};
    if (!core->Info (info, error))
        return false;
    const bool coreHasAtlas = info.displayStatus == 0;
    if (coreHasAtlas != hasAtlas ||
        (hasAtlas && (info.displaySize[0] != width || info.displaySize[1] != height || info.displayFaces != faces)) ||
        info.sampleCount != record.positions.size () / 3) {
        error = "the Rust study core does not match the record's atlas or samples; the study stays on the C++ core";
        return false;
    }
    record.rustCore = std::shared_ptr<rust::RustStudyCore> (std::move (core));
    return true;
#else
    (void) record;
    (void) snapshot;
    (void) previous;
    (void) maxAtlasDimension;
    error = "this build has no Rust sun-study core";
    return false;
#endif
}

} // namespace evp::sunstudy
