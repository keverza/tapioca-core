#ifndef EVP_SUNSTUDY_SUNSTUDYSTORE_HPP
#define EVP_SUNSTUDY_SUNSTUDYSTORE_HPP

// SunStudy/SunStudyStore — the live studies, addressed by id.
//
// A study outlives the call that starts it: that is the whole point of a session
// that is advanced rather than awaited. Something has to hold it between calls,
// and this is that thing.
//
// ⚠️ IT OWNS THE SAMPLE ARRAYS, AND THAT IS NOT INCIDENTAL. SampleSet is
// non-owning by design, because the analysis must be able to run over a caller's
// buffers without copying them. But a study advanced across many separate bus
// calls has no caller stack to borrow from — the arrays would be freed the
// moment the starting call returned, and every later slice would read released
// memory. So the store copies them once and hands out a SampleSet pointing at
// its own copy.
//
// ⚠️ EVERY ACCESS IS UNDER THE LOCK BECAUSE THE CALLERS ARE ON DIFFERENT
// THREADS. Starting a study needs the host's main thread (the sun comes from
// Archicad); advancing it deliberately does not, so that the work stays off that
// thread. Those are different threads by design, not by accident.
//
// ⚠️ THE LOCK IS NOT HELD ACROSS THE ANALYSIS ITSELF. Advancing a study is the
// expensive part; holding the store's mutex for it would serialise every other
// caller behind it, including a cheap progress poll from the UI. `Advance`
// therefore takes the lock, finds the session, and releases it — see the note on
// the method.

#include "Geometry/QueryEngine.hpp"
#include "SunStudy/CpuTraversal.hpp"
#include "SunStudy/SunStudyAtlas.hpp"
#include "SunStudy/SunStudyPatchAtlas.hpp"
#include "SunStudy/SunStudyPatchSampler.hpp"
#include "SunStudy/SunStudySession.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace evp::sunstudy {

// Everything one live study holds. Addressed only through the store.
struct StudyRecord {
    std::string id;

    // Owned copies; see the header note on why these are not borrowed.
    std::vector<double> positions;
    std::vector<double> normals;

    SunSeries series;
    SunStudySession session;
    std::shared_ptr<CpuTraversal> traversal;

    // Reported back so a caller can say what it studied without holding the
    // parameters itself.
    int timestepMinutes = 60;
    int year = 0, month = 0, day = 0;
    int hourFrom = 0, hourTo = 24;
    double minAltitudeDegrees = 0.0;
    double gridSpacing = 0.0;
    double groundPad = 0.0;

    // The element GUIDs the caller PICKED as analysis surfaces and as context,
    // exactly as given (see SunStudy/SunStudyRoles.hpp). Kept as asked rather
    // than as resolved, because a rerun resolves them again against a model
    // that may have gained or lost elements since.
    std::vector<std::string> analysisElements;
    std::vector<std::string> contextElements;
    std::vector<std::string> ignoredElements;

    // The roles AS RESOLVED, one per snapshot mesh (an ElementRole value), for
    // the display's role view. ⚠️ ALIGNED WITH THE SNAPSHOT THE STUDY RAN ON;
    // the display checks the length before trusting it.
    std::vector<uint8_t> elementRoles;
    size_t sourceStepCount = 0;

    // Wall-clock milliseconds spent inside Advance, summed. The measurement the
    // whole backend decision rests on, kept where a live run can read it.
    double analysisMilliseconds = 0.0;

    // The study's result as a texture, and the map from a point on the model
    // into it. Built once beside the samples, because it is a pure function of
    // them: rebuilding it per read would repack the atlas and invalidate every
    // texture coordinate a consumer had already been handed.
    SunStudyAtlas atlas;
    SampleGrid sampleGrid;

    // ---- the SurfacePatch domain ------------------------------------------
    //
    // ⚠️ BOTH DOMAINS LIVE IN ONE RECORD RATHER THAN IN TWO STUDY TYPES, and
    // only one of them is populated. The alternative -- a second record type
    // with its own store, advance and results path -- is how one analysis engine
    // becomes two, and the first defect fixed in either would live on in the
    // other. `domain` says which set of fields is real; `positions`, `normals`
    // and everything downstream of them are filled identically by both.
    SamplingDomain domain = SamplingDomain::TriangleLegacy;

    // Populated only when `domain == SurfacePatch`. The spans index the SAME
    // `positions`/`normals` the triangle path fills, which is exactly what lets
    // the accumulator and the traversal stay unaware of the difference.
    PatchSampleGrid patchGrid;

    // ⚠️ THE ATLAS IS OWNED BY THE STUDY AND OUTLIVES EVERY UPDATE. Its whole
    // value is that a patch's rectangle does not move; rebuilding it per read,
    // or holding it anywhere a rerun would replace it, throws away the one
    // property incremental display depends on.
    SunStudyPatchAtlas patchAtlas;

    bool IsPatchDomain () const
    {
        return domain == SamplingDomain::SurfacePatch;
    }

    SampleSet Samples () const;
};

// A patch-domain study expressed as the per-SOURCE-FACE arrays the display path
// already reads: every triangle a patch claimed gets that PATCH's lattice and
// that patch's atlas rectangle.
//
// ⚠️ THE RENDERER KEEPS ITS ONE RECORD PER DRAWN TRIANGLE, AND THAT IS THE
// POINT. The shader maps a world point through an origin, two axes, a start cell
// and a tile; a patch lattice is exactly those five things (start cell 0, origin
// at the patch's minimum corner). Handing every triangle of a surface the SAME
// record is what makes the checker continuous across source-triangle seams, and
// it leaves the shader, the binder and the topology hash untouched -- so the
// first live patch display tests the mapping and nothing else.
//
// ⚠️ THE TRIANGLE -> PATCH STEP IS `patchOfTriangle`, NEVER A REDISCOVERY. A
// triangle it does not claim (`kNoPatch`) gets an unplaced tile, which the
// shader draws as ordinary shading -- not patch 0's sunlight.
//
// ⚠️ A PATCH WITH ONE SAMPLE IS SHOWN AS A 1x1 TILE ON THAT SAMPLE'S TEXEL. The
// centroid fallback writes cell (0,0) of a lattice that may be larger; left at
// full size, every other cell would read the sentinel and the surface would
// vanish from the picture while its hours stayed in every total.
//
// Returns false when the grid and the atlas do not describe one study (an
// unplaced span, a mapping of the wrong length).
bool PatchFaceArrays (const PatchSampleGrid& grid, const SunStudyPatchAtlas& atlas, std::vector<AtlasTile>& tiles,
                      std::vector<FaceLayout>& layouts);

class SunStudyStore final {
  public:
    static SunStudyStore& Get ();

    // Takes ownership of `record`'s buffers and returns the id it was filed
    // under. An empty `record.id` gets a generated one.
    std::string Insert (std::unique_ptr<StudyRecord> record);

    // ⚠️ ADVANCING DOES NOT HOLD THE STORE'S LOCK. It takes the lock only to
    // resolve the id to a record, then releases it and works on the record
    // itself. Two callers advancing the SAME study concurrently is the one thing
    // that would race, and it is prevented by a per-record flag rather than by
    // serialising every unrelated caller behind the slow path.
    //
    // Returns false when the id is unknown or another thread is already
    // advancing that study; `error` says which.
    bool Advance (const std::string& id, size_t maxSteps, size_t maxParallel, double tmin, double tmax,
                  size_t& advanced, std::string& error);

    // A snapshot of one study's progress and parameters. False when unknown.
    bool Progress (const std::string& id, StudyProgress& progress, std::string& error) const;

    // Read under the lock and copy out, so a caller never holds a pointer into
    // a study another thread may erase.
    bool SunHours (const std::string& id, std::vector<double>& hours, std::vector<double>& positions,
                   std::string& error) const;

    // Everything a consumer needs to draw or diff a study, read under one lock
    // so the four arrays cannot come from different generations.
    //
    // ⚠️ `stepBits` IS THE REASON THE OTHER ENGINE CAN BE DIFFED AT ALL. Hours
    // alone answer "how much"; a per-sample-per-step bit answers "which steps",
    // which is what a sample-by-sample cross-check compares and what the
    // viewer's single-instant and AM/PM modes read. One byte per (sample, step)
    // on the wire, so it is opt-in.
    bool Results (const std::string& id, std::vector<double>& hours, std::vector<double>& positions,
                  std::vector<double>& normals, std::vector<uint8_t>* stepBits, std::string& error) const;

    // The hours as a texture image, `width * height` floats, with a negative
    // sentinel in every texel no sample landed on.
    bool AtlasImage (const std::string& id, uint32_t& width, uint32_t& height, std::vector<float>& image,
                     std::string& error) const;

    // The same, for a `domain=patch` study: the hours scattered into the PATCH
    // atlas, one rectangle per surface.
    //
    // ⚠️ A SEPARATE ENTRY POINT RATHER THAN A BRANCH INSIDE `AtlasImage`,
    // because the two images are not interchangeable even though both are
    // `width * height` floats with a negative sentinel. They are packed by
    // different allocators and addressed by different maps -- a consumer holding
    // triangle tile coordinates would read a patch image without complaint and
    // draw every face with a stranger's sunlight. Making the caller name which
    // one it wants is what keeps that from being a silent mistake.
    bool PatchAtlasImage (const std::string& id, uint32_t& width, uint32_t& height, std::vector<float>& image,
                          std::string& error) const;

    // Everything a RENDERER needs, read under one lock so the image and the
    // packing it was scattered through cannot come from different generations.
    //
    // ⚠️ THE PER-FACE ARRAYS AND NOT THE SAMPLE ARRAYS. A display path needs the
    // tiles, the face layouts and the image; it does not need the positions,
    // normals, areas or step bits, which on a real model are tens of megabytes.
    // Copying those to change a palette is the one way this could cost a frame.
    //
    // A patch-domain study answers in the same shape, through PatchFaceArrays
    // and the patch atlas image: the tiles are per source face either way, so a
    // caller cannot pair one domain's tiles with the other's image.
    //
    // ⚠️ `converged` IS RETURNED RATHER THAN CHECKED HERE. A study still
    // advancing has an atlas of the hours SO FAR, which are too low; that is a
    // legitimate thing to draw progressively and an illegitimate thing to
    // present as a finished study, and only the caller knows which it is doing.
    bool DisplayData (const std::string& id, std::vector<AtlasTile>& tiles, std::vector<FaceLayout>& layouts,
                      uint32_t& width, uint32_t& height, double& spacing, std::vector<float>& image,
                      double& daylightHours, bool& converged, uint64_t& generation, std::string& error) const;

    bool Describe (const std::string& id, StudyRecord& copyOfMetadata, std::string& error) const;

    bool Erase (const std::string& id);
    void Clear ();

    std::vector<std::string> Ids () const;
    size_t Count () const;

  private:
    SunStudyStore () = default;

    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<StudyRecord>> studies_;
    std::map<std::string, bool> advancing_;
    uint64_t nextId_ = 1;
};

} // namespace evp::sunstudy

#endif
