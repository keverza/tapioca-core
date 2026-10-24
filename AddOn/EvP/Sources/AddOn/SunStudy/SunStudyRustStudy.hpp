#ifndef EVP_SUNSTUDY_SUNSTUDYRUSTSTUDY_HPP
#define EVP_SUNSTUDY_SUNSTUDYRUSTSTUDY_HPP

// SunStudy/SunStudyRustStudy -- the Rust study core: the portable half of a
// split StudyRecord (issue #3, option A).
//
// The record keeps the snapshot, traversal, occluders, display cache, binding,
// timings and the store's id map, revisions and locks; this owns the day, the
// samples, the session, the atlas the result is drawn through and the
// sample-to-mesh map. Its readers answer what SunStudyStore's readers answer,
// in the C++ core's own types, so the flip swaps an implementation and not a
// vocabulary. The store's "computing" refusal while a slice runs stays with
// the store, which owns the advancing flag.
//
// Lifetime: Create, SetSampleMeshes and ReuseFrom run before the core is
// shared; afterwards every member is const and safe to call from any thread.

#include "SunStudy/SunStudyRust.hpp"
#include "SunStudy/SunStudyReading.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv {
struct Snapshot;
}

namespace evp::sunstudy::rust {

// Why a display read was refused; a refusal is an answer, not an error.
enum class StudyReadStatus : uint8_t {
    Ok = 0,
    NoTriangleAtlas = 1, // no model-surface (triangle) atlas
    NoPatchAtlas = 2,    // not a patch-domain study with a packed atlas
    UnplacedPatch = 3,   // grid and atlas are not one study
};

class RustStudyCore final {
  public:
    using StepObserver = std::function<void (size_t, const TapiocaOcclusionState&, double)>;

    struct TriangleInputs {
        const SampleGrid* grid = nullptr;            // built with layouts
        const geomsrv::Snapshot* snapshot = nullptr; // stable tile identities
        AtlasOptions options;
    };

    // Null with `error` set when refused: inconsistent samples, or (patch) an
    // atlas that cannot be packed within `maxDimension`. `previous` is the
    // study being rerun; its allocations are kept under the C++ conditions.
    static std::unique_ptr<RustStudyCore> Ground (const SunSeries& series, double spacing,
                                                  const std::vector<double>& positions,
                                                  const std::vector<double>& normals, std::string& error);
    static std::unique_ptr<RustStudyCore> Triangle (const SunSeries& series, double spacing,
                                                    const TriangleInputs& inputs, const RustStudyCore* previous,
                                                    std::string& error);
    static std::unique_ptr<RustStudyCore> Patch (const SunSeries& series, double spacing, const PatchSampleGrid& grid,
                                                 const RustStudyCore* previous, uint32_t maxDimension,
                                                 std::string& error);

    ~RustStudyCore ();
    RustStudyCore (const RustStudyCore&) = delete;
    RustStudyCore& operator= (const RustStudyCore&) = delete;

    // ---- preparation (before the core is shared) --------------------------
    bool Sync (const StudyInputs& inputs, std::string& error) const;
    bool SetSampleMeshes (const geomsrv::Snapshot& snapshot, std::string& error);

    struct ReuseInputs {
        const geomsrv::Snapshot* sourceSnapshot = nullptr;
        const std::vector<uint8_t>* sourceRoles = nullptr;
        const geomsrv::Snapshot* targetSnapshot = nullptr;
        const std::vector<uint8_t>* targetRoles = nullptr;
        bool sourceDefaultRayBounds = true;
        const std::atomic<bool>* cancelled = nullptr;
    };
    // ReuseUnaffectedSamples; `reusedSamples` 0 when nothing was seeded.
    bool ReuseFrom (const RustStudyCore& source, const ReuseInputs& inputs, size_t& reusedSamples,
                    size_t& reusedSampleSteps, std::string& error);

    // ---- the study ------------------------------------------------------
    bool Advance (const ITraversal& traversal, size_t maxSteps, double tmin, double tmax, size_t maxParallel,
                  const std::function<bool ()>& isCancelled, const StepObserver& observer, size_t& advanced,
                  std::string& error) const;
    bool Progress (StudyProgress& progress, std::string& error) const;
    bool Info (TapiocaStudyInfo& info, std::string& error) const;

    bool SunHours (std::vector<double>& hours, std::string& error) const;
    // One byte per (sample, step), sample-major: the store's Results wire.
    bool StepBits (std::vector<uint8_t>& bits, std::string& error) const;
    bool SampleMeshes (std::vector<uint32_t>& meshes, std::string& error) const;
    bool Summary (SunStudyResultSummary& summary, std::string& error) const;

    bool AtlasImage (uint32_t& width, uint32_t& height, std::vector<float>& image, StudyReadStatus& status,
                     std::string& error) const;
    bool PatchAtlasImage (uint32_t& width, uint32_t& height, std::vector<float>& image, StudyReadStatus& status,
                          std::string& error) const;
    bool DisplayData (std::vector<AtlasTile>& tiles, std::vector<FaceLayout>& layouts, uint32_t& width,
                      uint32_t& height, std::vector<float>& image, StudyReadStatus& status, std::string& error) const;
    bool StepMasks (StepMaskAtlas& atlas, std::vector<uint16_t>& stepMinutes, uint32_t& noonStep,
                    StudyReadStatus& status, std::string& error) const;
    bool ReadAt (size_t face, const double point[3], SunStudyReading& reading, std::string& error) const;

  private:
    explicit RustStudyCore (TapiocaSunStudyStudy* handle);
    bool Image (uint8_t kind, std::vector<AtlasTile>* tiles, std::vector<FaceLayout>* layouts, uint32_t& width,
                uint32_t& height, std::vector<float>& image, StudyReadStatus& status, std::string& error) const;

    TapiocaSunStudyStudy* handle_ = nullptr;
};

} // namespace evp::sunstudy::rust

#endif
