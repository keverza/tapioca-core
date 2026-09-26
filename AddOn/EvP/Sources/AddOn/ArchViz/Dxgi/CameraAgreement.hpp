#ifndef EVP_ARCHVIZ_DXGI_CAMERAAGREEMENT_HPP
#define EVP_ARCHVIZ_DXGI_CAMERAAGREEMENT_HPP

// ArchViz/Dxgi/CameraAgreement -- within one scene image, which draws carry the
// camera the model's own draws used, the previous image's, or neither?
// Bound by private/docs/architecture/diligent/OVERLAY-INVARIANTS.md: observation only;
// it never selects, copies for, or gates the overlay's camera.
//
// WHY IT EXISTS (Stage 73). Stage 72 proved the overlay reads the camera copied
// in the same image that reaches the back buffer, yet the overlay still trails
// the model while navigating and is exact at rest. The census ranks candidate
// draws by how well their camera projects the model; a camera one image stale
// projects it just as well, so calibration cannot tell fresh from stale.
//
// ⚠️ WHY IT COMPARES POSES, NOT BYTES (Stage 74). Stage 73 compared the first 64
// bytes of b1 against the first draw with more than six indices. Its run showed
// the draws hold DIFFERENT KINDS of matrix: that first draw is a rotation-only
// background whose b1 is a combined view-projection with no translation, the
// model draws carry combined view-projections in b1 and zeros in b2, and the
// verified draw carries a separate view (b1) and projection (b2). Bytes never
// compare across those forms, so every position read OTHER and the verdict could
// not be read. This version classifies each window's form, reads a camera POSE
// out of any form that has one -- eye, view z axis and view x axis in world
// coordinates -- and compares ROTATIONS, which every form shares: an orbit turns
// the camera every image, so one image of staleness is a rotation step.
//
// It also keeps the raw first 64 bytes of b0..b3 of every draw of a few
// consecutive images, so the forms can be checked by hand against Intel GPA.

#include <cstddef>
#include <cstdint>

struct ID3D11DeviceContext;
struct IDXGISwapChain;

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace cameraagreement {

constexpr size_t kSlots = 8;               // images in flight to the CPU
constexpr size_t kMaxDraws = 32;           // Archicad draws recorded per image, in GPU order
constexpr size_t kWindows = 4;             // b0..b3, the first 64 bytes of each
constexpr size_t kFloatsPerWindow = 16;    // one 4x4 matrix
constexpr size_t kMaxKeys = 24;            // distinct draws tracked across images
constexpr size_t kDumpImages = 4;          // consecutive images whose raw windows are kept
constexpr uint64_t kDumpFirstImage = 8;    // the dump starts at this image read, counted from 0
constexpr uint32_t kModelMinimumCount = 7; // more indices than a quad: model geometry

// ⚠️ ORDER IS THE WIRE FORMAT: `formsB1`/`formsB2` are indexed by it and the
// OverlayRegression diagnostic names them in this order.
//   ROW_VECTOR    -- registers are rows, `mul (p, M)`; translation in register 3
//   COLUMN_VECTOR -- registers are columns of that, i.e. the transpose in memory
enum class Form : uint32_t {
    Unbound = 0,
    Zero,
    ViewRowVector,
    ViewColumnVector,
    ViewProjectionRowVector,
    ViewProjectionColumnVector,
    Projection, // no rotation: a perspective, an orthographic or a pixel-to-NDC map
    Other,
    Count
};
constexpr size_t kFormCount = size_t (Form::Count);

// A draw is identified across images by its count and how many earlier draws of
// the same image had the same count -- positions shift when a draw is culled,
// this does not.
struct KeyStats {
    uint32_t count = 0;
    uint32_t ordinal = 0;
    uint32_t kind = 0; // the post-draw kind: 0 indexed, 1 direct, 2/3 instanced
    uint64_t seen = 0;
    uint64_t posed = 0;      // images with a camera pose read from this draw
    int32_t poseWindow = -1; // the window the last pose came from; -1 none
    uint64_t formsB1[kFormCount] = {};
    uint64_t formsB2[kFormCount] = {};
    uint64_t moving = 0;       // posed here and in the previous image, rotation changed
    uint64_t rootSame = 0;     // moving: the verified draw's rotation equals this draw's, same image
    uint64_t rootPrevious = 0; // moving: it equals this draw's PREVIOUS image -- the verified draw lags
    uint64_t rootAhead = 0;    // moving: the verified draw's previous image equals this draw's -- it leads
    uint64_t rootNeither = 0;
    // This draw against the image's reference, in the images where the reference
    // moved: PREVIOUS means this draw carries the model family's previous camera.
    uint64_t referenceMoving = 0;
    uint64_t vsReferenceSame = 0;
    uint64_t vsReferencePrevious = 0;
    uint64_t vsReferenceAhead = 0;
    uint64_t vsReferenceNeither = 0;
    uint64_t wasRoot = 0;
    uint64_t wasReference = 0;
    double stepSumDegrees = 0.0;      // moving: this draw's own rotation change per image
    double rootAngleSumDegrees = 0.0; // moving: rotation between this draw and the verified draw, same image
    double rootEyeSum = 0.0;          // moving, both with an eye: eye distance to the verified draw
    uint64_t rootEyeSamples = 0;
};

struct Stats {
    bool enabled = false;
    uint64_t imagesOpened = 0;
    uint64_t imagesSkipped = 0; // no free slot when the image began
    uint64_t imagesRead = 0;
    uint64_t imagesConsecutive = 0; // read, and the previous image was read too
    uint64_t drawsTruncated = 0;
    uint64_t keysTruncated = 0;
    uint64_t readbacksPending = 0; // DO_NOT_WAIT refusals; retried, not a fault
    uint64_t readbackFailures = 0; // Map failed outright; the image is dropped, counted
    uint64_t createFailures = 0;
    uint64_t rootMissing = 0;      // images read with no verified draw
    uint64_t rootPoseMissing = 0;  // a verified draw whose windows hold no pose
    uint64_t referenceMissing = 0; // images read with no model-family view-projection draw
    // The headline: the verified draw against the reference -- the first draw of
    // the image with more than six indices whose b1 is a view-projection.
    uint64_t imagesMoving = 0; // consecutive, reference posed in both, its rotation changed
    uint64_t rootSame = 0;
    uint64_t rootPrevious = 0;
    uint64_t rootAhead = 0;
    uint64_t rootNeither = 0;
    double rootAngleSumDegrees = 0.0;
    double referenceStepSumDegrees = 0.0;
    uint32_t keyCount = 0;
    KeyStats keys[kMaxKeys];
    uint32_t dumpCount = 0;
};

struct DumpWindow {
    bool bound = false;
    uint32_t form = 0;
    uint64_t buffer = 0;
    uint32_t firstConstant = 0;
    uint32_t numConstants = 0;
    uint32_t bytesCopied = 0;
    float values[kFloatsPerWindow] = {};
};

struct DumpDraw {
    uint32_t count = 0;
    uint32_t ordinal = 0;
    uint32_t kind = 0;
    bool root = false;
    bool reference = false;
    uint64_t vertexShader = 0;
    uint64_t renderTarget = 0;
    DumpWindow windows[kWindows];
};

struct DumpImage {
    uint64_t generation = 0;
    uint32_t draws = 0;
    DumpDraw draw[kMaxDraws];
};

// MAIN THREAD. Disabled before the shared PassProvenance drain; Reset after it.
void SetEnabled (bool enabled);
bool Enabled ();
void Reset ();

// CONTEXT THREAD, from the draw detours' common post-draw path, for Archicad's
// draws only, after SceneCameraPairing has committed this draw's root (if any).
void OnDrawCompleted (ID3D11DeviceContext* context, uint32_t kind, uint32_t count);

// PRESENT THREAD, nominated chain, inside the active PassProvenance Present
// interval. Reads back at most one finished image per call, never waits.
void OnPresent (IDXGISwapChain* swapChain);

Stats GetStats ();

// MAIN THREAD. Copies the published raw images, oldest first; returns how many.
size_t CopyDump (DumpImage* out, size_t capacity);

} // namespace cameraagreement
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
