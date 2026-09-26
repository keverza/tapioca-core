// ArchViz/Dxgi/CameraAgreement -- per image, the camera pose of every Archicad draw against the verified draw's
// and the model family's. Bound by private/docs/architecture/diligent/OVERLAY-INVARIANTS.md: observation only,
// GPU copies under ScopedInjectionGuard, DO_NOT_WAIT readback, no locks, no heap after the one-time staging creation.

#include "ArchViz/Dxgi/CameraAgreement.hpp"

#include "ArchViz/Dxgi/ContextStateTracker.hpp"
#include "ArchViz/Dxgi/SceneCameraPairing.hpp"

#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace cameraagreement {

namespace {

constexpr uint32_t kBytesPerWindow = uint32_t (kFloatsPerWindow * sizeof (float));
constexpr uint32_t kBytesPerDraw = uint32_t (kWindows) * kBytesPerWindow;
constexpr double kDegreesPerRadian = 57.29577951308232;

// ⚠️ TWO THRESHOLDS, AN ORDER OF MAGNITUDE APART. The same camera read through
// two forms agrees to float rounding, about 1e-6 rad; an orbit turns it by a
// hundredth of a radian or more per image. A rotation within kMatchRadians is the
// same camera and a change of kMovingRadians or more is a moved one, so no pose
// can equal both an image's camera and the next image's.
constexpr double kMatchRadians = 2e-4;  // 0.011 degrees
constexpr double kMovingRadians = 1e-3; // 0.057 degrees

enum class SlotState : uint32_t { Free = 0, Filling = 1, Submitted = 2 };

struct DrawMeta {
    uint32_t count = 0;
    uint32_t ordinal = 0;
    uint32_t kind = 0;
    bool root = false;
    uint64_t vertexShader = 0;
    uint64_t renderTarget = 0;
    contextstate::ConstantBufferBinding windows[kWindows];
    uint32_t bytesCopied[kWindows] = {};
};

// One image on its way to the CPU. The context thread owns every plain field
// while the slot is Filling and publishes them with the release store of
// Submitted; the Present thread reads them after its acquire of Submitted and
// hands the slot back with the release store of Free.
struct Slot {
    std::atomic<uint32_t> state { uint32_t (SlotState::Free) };
    ID3D11Buffer* staging = nullptr;
    uint64_t generation = 0;
    uint64_t sequence = 0;
    uint32_t draws = 0;
    DrawMeta meta[kMaxDraws];
};

// A camera pose in world coordinates. The eye is absent for a rotation-only form
// whose translation cannot be solved for.
struct Pose {
    bool valid = false;
    bool hasEye = false;
    double eye[3] = {};
    double z[3] = {}; // the view z axis, unit
    double x[3] = {}; // the view x axis, unit
};

// Written by the Present thread only; read on the main thread, torn-free per field.
struct KeyCounters {
    std::atomic<uint32_t> count { 0 };
    std::atomic<uint32_t> ordinal { 0 };
    std::atomic<uint32_t> kind { 0 };
    std::atomic<uint64_t> seen { 0 };
    std::atomic<uint64_t> posed { 0 };
    std::atomic<int32_t> poseWindow { -1 };
    std::atomic<uint64_t> formsB1[kFormCount]; // static storage: zero-initialised
    std::atomic<uint64_t> formsB2[kFormCount];
    std::atomic<uint64_t> moving { 0 };
    std::atomic<uint64_t> rootSame { 0 };
    std::atomic<uint64_t> rootPrevious { 0 };
    std::atomic<uint64_t> rootAhead { 0 };
    std::atomic<uint64_t> rootNeither { 0 };
    std::atomic<uint64_t> referenceMoving { 0 };
    std::atomic<uint64_t> vsReferenceSame { 0 };
    std::atomic<uint64_t> vsReferencePrevious { 0 };
    std::atomic<uint64_t> vsReferenceAhead { 0 };
    std::atomic<uint64_t> vsReferenceNeither { 0 };
    std::atomic<uint64_t> wasRoot { 0 };
    std::atomic<uint64_t> wasReference { 0 };
    std::atomic<double> stepSumDegrees { 0.0 };
    std::atomic<double> rootAngleSumDegrees { 0.0 };
    std::atomic<double> rootEyeSum { 0.0 };
    std::atomic<uint64_t> rootEyeSamples { 0 };
};

// The Present thread's memory of the previous image, per key and for the two
// reference points. Only poses from generation g - 1 are ever compared.
struct History {
    uint64_t generation = 0;
    Pose pose;
};

std::atomic<bool> g_enabled { false };
Slot g_slots[kSlots];

// Context thread.
bool g_created = false;
bool g_createFailed = false;
int g_openSlot = -1;
uint64_t g_openGeneration = 0;
uint64_t g_lastRootCommits = 0;
uint64_t g_nextSequence = 0;

// Present thread.
bool g_lastReadValid = false;
uint64_t g_lastReadGeneration = 0;
History g_keyHistory[kMaxKeys];
History g_rootHistory;
History g_referenceHistory;

// Written by one thread each, read on the main thread after the drain.
std::atomic<uint64_t> g_imagesOpened { 0 };
std::atomic<uint64_t> g_imagesSkipped { 0 };
std::atomic<uint64_t> g_drawsTruncated { 0 };
std::atomic<uint64_t> g_createFailures { 0 };
std::atomic<uint64_t> g_imagesRead { 0 };
std::atomic<uint64_t> g_imagesConsecutive { 0 };
std::atomic<uint64_t> g_keysTruncated { 0 };
std::atomic<uint64_t> g_readbacksPending { 0 };
std::atomic<uint64_t> g_readbackFailures { 0 };
std::atomic<uint64_t> g_rootMissing { 0 };
std::atomic<uint64_t> g_rootPoseMissing { 0 };
std::atomic<uint64_t> g_referenceMissing { 0 };
std::atomic<uint64_t> g_imagesMoving { 0 };
std::atomic<uint64_t> g_rootSame { 0 };
std::atomic<uint64_t> g_rootPrevious { 0 };
std::atomic<uint64_t> g_rootAhead { 0 };
std::atomic<uint64_t> g_rootNeither { 0 };
std::atomic<double> g_rootAngleSumDegrees { 0.0 };
std::atomic<double> g_referenceStepSumDegrees { 0.0 };
std::atomic<uint32_t> g_keyCount { 0 };
KeyCounters g_keys[kMaxKeys];

// ⚠️ PUBLISHED ONCE PER INDEX. The Present thread fills image i completely and
// then stores i + 1 with release; the main thread copies only below what it
// acquired, and nothing rewrites a published image until Reset.
DumpImage g_dump[kDumpImages];
std::atomic<uint32_t> g_dumpCount { 0 };

// Single writer, so a load and a store is an add.
void Accumulate (std::atomic<double>& target, double value)
{
    target.store (target.load (std::memory_order_relaxed) + value, std::memory_order_relaxed);
}

double Dot (const double* a, const double* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

double Length (const double* a)
{
    return std::sqrt (Dot (a, a));
}

bool Normalize (double* a)
{
    const double length = Length (a);
    if (!(length > 1e-12))
        return false;
    a[0] /= length;
    a[1] /= length;
    a[2] /= length;
    return true;
}

// Sign-agnostic: a form may carry the axis negated, and that is the same camera.
double AxisAngle (const double* a, const double* b)
{
    const double cross[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    return std::atan2 (Length (cross), std::fabs (Dot (a, b)));
}

// Any rotation, about any axis, moves the view z axis, the view x axis, or both.
double RotationDifference (const Pose& a, const Pose& b)
{
    const double z = AxisAngle (a.z, b.z);
    const double x = AxisAngle (a.x, b.x);
    return z > x ? z : x;
}

double EyeDistance (const Pose& a, const Pose& b)
{
    const double d[3] = { a.eye[0] - b.eye[0], a.eye[1] - b.eye[1], a.eye[2] - b.eye[2] };
    return Length (d);
}

// Element (r, c) of the matrix in ROW-VECTOR form, `out = p * M`. A window whose
// registers are the columns of that -- the transpose in memory -- is read with
// `columnVector` set, so every test below is written once.
double At (const float* m, bool columnVector, int r, int c)
{
    return double (columnVector ? m[4 * c + r] : m[4 * r + c]);
}

bool Solve3 (const double a[3][3], const double b[3], double out[3])
{
    const auto det3 = [] (const double m[3][3]) {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    };
    const double det = det3 (a);
    const double scale = Length (a[0]) * Length (a[1]) * Length (a[2]);
    if (!(std::fabs (det) > 1e-9 * scale))
        return false;
    for (int column = 0; column < 3; ++column) {
        double replaced[3][3];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                replaced[r][c] = c == column ? b[r] : a[r][c];
        out[column] = det3 (replaced) / det;
    }
    return true;
}

// A view: `p_view = p * A + t`, A a rotation times one uniform scale, the last
// column (0, 0, 0, 1). The eye maps to the view origin, so eye * A = -t.
bool ReadView (const float* m, bool columnVector, Pose& pose)
{
    if (std::fabs (At (m, columnVector, 0, 3)) > 1e-5 || std::fabs (At (m, columnVector, 1, 3)) > 1e-5 ||
        std::fabs (At (m, columnVector, 2, 3)) > 1e-5 || std::fabs (At (m, columnVector, 3, 3) - 1.0) > 1e-5)
        return false;
    double row[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            row[r][c] = At (m, columnVector, r, c);
    const double n0 = Dot (row[0], row[0]);
    const double n1 = Dot (row[1], row[1]);
    const double n2 = Dot (row[2], row[2]);
    const double scale2 = (n0 + n1 + n2) / 3.0;
    if (!(scale2 > 1e-12))
        return false;
    if (std::fabs (n0 - scale2) > 1e-3 * scale2 || std::fabs (n1 - scale2) > 1e-3 * scale2 ||
        std::fabs (n2 - scale2) > 1e-3 * scale2)
        return false;
    if (std::fabs (Dot (row[0], row[1])) > 1e-3 * scale2 || std::fabs (Dot (row[0], row[2])) > 1e-3 * scale2 ||
        std::fabs (Dot (row[1], row[2])) > 1e-3 * scale2)
        return false;
    const double t[3] = { At (m, columnVector, 3, 0), At (m, columnVector, 3, 1), At (m, columnVector, 3, 2) };
    for (int r = 0; r < 3; ++r)
        pose.eye[r] = -(t[0] * row[r][0] + t[1] * row[r][1] + t[2] * row[r][2]) / scale2;
    // Column 2 of A is the view z axis in world coordinates, column 0 the x axis.
    for (int r = 0; r < 3; ++r) {
        pose.z[r] = row[r][2];
        pose.x[r] = row[r][0];
    }
    if (!Normalize (pose.z) || !Normalize (pose.x))
        return false;
    pose.valid = true;
    pose.hasEye = true;
    return true;
}

// A perspective view-projection, `clip = p * M`. Clip w is -z_view, so column 3
// is the view z axis (negated), and the depth column 2 is parallel to it. The eye
// is where clip x, y and w all vanish.
bool ReadViewProjection (const float* m, bool columnVector, Pose& pose)
{
    double column[4][3];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 3; ++r)
            column[c][r] = At (m, columnVector, r, c);
    const double n2 = Length (column[2]);
    const double n3 = Length (column[3]);
    if (!(n2 > 1e-9) || !(n3 > 1e-9))
        return false;
    if (std::fabs (Dot (column[2], column[3])) / (n2 * n3) < 1.0 - 1e-6)
        return false;
    for (int r = 0; r < 3; ++r)
        pose.z[r] = -column[3][r] / n3;
    const double along = Dot (column[0], pose.z);
    for (int r = 0; r < 3; ++r)
        pose.x[r] = column[0][r] - along * pose.z[r];
    if (!Normalize (pose.x))
        return false;
    const double system[3][3] = { { column[0][0], column[0][1], column[0][2] },
                                  { column[1][0], column[1][1], column[1][2] },
                                  { column[3][0], column[3][1], column[3][2] } };
    const double rhs[3] = { -At (m, columnVector, 3, 0), -At (m, columnVector, 3, 1), -At (m, columnVector, 3, 3) };
    pose.hasEye = Solve3 (system, rhs, pose.eye);
    pose.valid = true;
    return true;
}

// No rotation to read: the upper 3x3 is diagonal in either layout.
bool IsProjection (const float* m)
{
    double largest = 0.0;
    for (size_t i = 0; i < kFloatsPerWindow; ++i)
        largest = std::fabs (m[i]) > largest ? std::fabs (m[i]) : largest;
    const double tolerance = 1e-6 * largest;
    const int offDiagonal[6] = { 1, 2, 4, 6, 8, 9 };
    for (int index : offDiagonal)
        if (std::fabs (m[index]) > tolerance)
            return false;
    return std::fabs (m[0]) > tolerance && std::fabs (m[5]) > tolerance;
}

Form Classify (const float* m)
{
    bool zero = true;
    for (size_t i = 0; i < kFloatsPerWindow; ++i) {
        if (!std::isfinite (m[i]))
            return Form::Other;
        if (m[i] != 0.0f)
            zero = false;
    }
    if (zero)
        return Form::Zero;
    Pose scratch;
    if (ReadView (m, false, scratch))
        return Form::ViewRowVector;
    if (ReadView (m, true, scratch))
        return Form::ViewColumnVector;
    if (IsProjection (m))
        return Form::Projection;
    if (ReadViewProjection (m, false, scratch))
        return Form::ViewProjectionRowVector;
    if (ReadViewProjection (m, true, scratch))
        return Form::ViewProjectionColumnVector;
    return Form::Other;
}

bool ReadPose (const float* m, Form form, Pose& pose)
{
    switch (form) {
        case Form::ViewRowVector:
            return ReadView (m, false, pose);
        case Form::ViewColumnVector:
            return ReadView (m, true, pose);
        case Form::ViewProjectionRowVector:
            return ReadViewProjection (m, false, pose);
        case Form::ViewProjectionColumnVector:
            return ReadViewProjection (m, true, pose);
        default:
            return false;
    }
}

bool IsViewProjection (Form form)
{
    return form == Form::ViewProjectionRowVector || form == Form::ViewProjectionColumnVector;
}

// X against Y at image g: SAME if X(g) is Y(g); PREVIOUS if X(g) is Y(g - 1),
// X lagging; AHEAD if X(g - 1) is Y(g), X leading; otherwise NEITHER.
enum class Relation { Same, Previous, Ahead, Neither };

Relation Relate (const Pose& x, const History& xPrevious, const Pose& y, const History& yPrevious, uint64_t generation)
{
    if (RotationDifference (x, y) <= kMatchRadians)
        return Relation::Same;
    if (yPrevious.generation + 1 == generation && yPrevious.pose.valid &&
        RotationDifference (x, yPrevious.pose) <= kMatchRadians)
        return Relation::Previous;
    if (xPrevious.generation + 1 == generation && xPrevious.pose.valid &&
        RotationDifference (xPrevious.pose, y) <= kMatchRadians)
        return Relation::Ahead;
    return Relation::Neither;
}

void Tally (Relation relation, std::atomic<uint64_t>& same, std::atomic<uint64_t>& previous,
            std::atomic<uint64_t>& ahead, std::atomic<uint64_t>& neither)
{
    std::atomic<uint64_t>& target = relation == Relation::Same       ? same
                                    : relation == Relation::Previous ? previous
                                    : relation == Relation::Ahead    ? ahead
                                                                     : neither;
    target.fetch_add (1, std::memory_order_relaxed);
}

// Present thread. The key table only grows while enabled; its size is published
// with release after the identity fields, so a reader never sees a half-made key.
int FindOrAddKey (uint32_t count, uint32_t ordinal, uint32_t kind)
{
    const uint32_t known = g_keyCount.load (std::memory_order_relaxed);
    for (uint32_t j = 0; j < known; ++j) {
        const KeyCounters& key = g_keys[j];
        if (key.count.load (std::memory_order_relaxed) == count &&
            key.ordinal.load (std::memory_order_relaxed) == ordinal &&
            key.kind.load (std::memory_order_relaxed) == kind)
            return int (j);
    }
    if (known >= kMaxKeys) {
        g_keysTruncated.fetch_add (1, std::memory_order_relaxed);
        return -1;
    }
    KeyCounters& key = g_keys[known];
    key.count.store (count, std::memory_order_relaxed);
    key.ordinal.store (ordinal, std::memory_order_relaxed);
    key.kind.store (kind, std::memory_order_relaxed);
    g_keyHistory[known] = History {};
    g_keyCount.store (known + 1, std::memory_order_release);
    return int (known);
}

// CONTEXT THREAD. One-time creation of the staging ring on the first draw after
// enable -- the same first-use allocation InjectionCamera makes for its own
// snapshot and staging buffers. Never retried after a failure.
bool EnsureCreated (ID3D11DeviceContext* context)
{
    if (g_created)
        return true;
    if (g_createFailed)
        return false;
    ID3D11Device* device = nullptr;
    context->GetDevice (&device);
    if (device == nullptr) {
        g_createFailed = true;
        g_createFailures.fetch_add (1, std::memory_order_relaxed);
        return false;
    }
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = UINT (kMaxDraws * kBytesPerDraw);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bool ok = true;
    for (Slot& slot : g_slots)
        ok = ok && SUCCEEDED (device->CreateBuffer (&desc, nullptr, &slot.staging));
    device->Release ();
    if (!ok) {
        g_createFailed = true;
        g_createFailures.fetch_add (1, std::memory_order_relaxed);
        return false;
    }
    g_created = true;
    return true;
}

// CONTEXT THREAD. The image in progress is over; hand its slot to the Present thread.
void SubmitOpenSlot ()
{
    if (g_openSlot < 0)
        return;
    g_slots[g_openSlot].state.store (uint32_t (SlotState::Submitted), std::memory_order_release);
    g_openSlot = -1;
}

// CONTEXT THREAD. A new image began; take a free slot for it or count the skip.
void OpenSlot (ID3D11DeviceContext* context, uint64_t generation)
{
    if (!EnsureCreated (context))
        return;
    for (int i = 0; i < int (kSlots); ++i) {
        Slot& slot = g_slots[i];
        if (slot.state.load (std::memory_order_acquire) != uint32_t (SlotState::Free))
            continue;
        slot.generation = generation;
        slot.sequence = ++g_nextSequence;
        slot.draws = 0;
        slot.state.store (uint32_t (SlotState::Filling), std::memory_order_relaxed);
        g_openSlot = i;
        g_imagesOpened.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    g_imagesSkipped.fetch_add (1, std::memory_order_relaxed);
}

using ImageData = float[kMaxDraws][kWindows][kFloatsPerWindow];

// PRESENT THREAD. Classify every window of one read-back image, compare the
// poses, and keep the image's raw windows if it falls in the dump range.
void Evaluate (const Slot& slot, const ImageData& data)
{
    const uint64_t readIndex = g_imagesRead.fetch_add (1, std::memory_order_relaxed);
    const uint64_t generation = slot.generation;
    const bool consecutive = g_lastReadValid && g_lastReadGeneration + 1 == generation;
    if (consecutive)
        g_imagesConsecutive.fetch_add (1, std::memory_order_relaxed);

    Form forms[kMaxDraws][kWindows];
    Pose poses[kMaxDraws];
    int poseWindows[kMaxDraws];
    int keys[kMaxDraws];
    int root = -1;
    int reference = -1;
    const int windowOrder[kWindows] = { 1, 2, 0, 3 };
    for (uint32_t k = 0; k < slot.draws; ++k) {
        const DrawMeta& meta = slot.meta[k];
        for (size_t s = 0; s < kWindows; ++s) {
            if (!meta.windows[s].IsBound ())
                forms[k][s] = Form::Unbound;
            else if (meta.bytesCopied[s] < kBytesPerWindow)
                forms[k][s] = Form::Other; // too small to hold a matrix
            else
                forms[k][s] = Classify (data[k][s]);
        }
        poses[k] = Pose {};
        poseWindows[k] = -1;
        for (int window : windowOrder) {
            if (ReadPose (data[k][window], forms[k][window], poses[k])) {
                poseWindows[k] = window;
                break;
            }
            poses[k] = Pose {};
        }
        keys[k] = FindOrAddKey (meta.count, meta.ordinal, meta.kind);
        if (meta.root)
            root = int (k);
        if (reference < 0 && meta.count >= kModelMinimumCount && IsViewProjection (forms[k][1]) && poses[k].valid &&
            poseWindows[k] == 1)
            reference = int (k);
    }
    if (root < 0)
        g_rootMissing.fetch_add (1, std::memory_order_relaxed);
    else if (!poses[root].valid)
        g_rootPoseMissing.fetch_add (1, std::memory_order_relaxed);
    if (reference < 0)
        g_referenceMissing.fetch_add (1, std::memory_order_relaxed);
    const bool rootPosed = root >= 0 && poses[root].valid;
    const bool referencePosed = reference >= 0 && poses[reference].valid;

    // The headline: the verified draw against the model family, in the images
    // where the model family's camera moved.
    const bool referenceMoved = referencePosed && g_referenceHistory.generation + 1 == generation &&
                                g_referenceHistory.pose.valid &&
                                RotationDifference (poses[reference], g_referenceHistory.pose) >= kMovingRadians;
    if (referenceMoved) {
        g_imagesMoving.fetch_add (1, std::memory_order_relaxed);
        Accumulate (g_referenceStepSumDegrees,
                    RotationDifference (poses[reference], g_referenceHistory.pose) * kDegreesPerRadian);
        if (rootPosed) {
            Tally (Relate (poses[root], g_rootHistory, poses[reference], g_referenceHistory, generation), g_rootSame,
                   g_rootPrevious, g_rootAhead, g_rootNeither);
            Accumulate (g_rootAngleSumDegrees, RotationDifference (poses[root], poses[reference]) * kDegreesPerRadian);
        }
    }

    for (uint32_t k = 0; k < slot.draws; ++k) {
        if (keys[k] < 0)
            continue;
        KeyCounters& key = g_keys[keys[k]];
        const History& history = g_keyHistory[keys[k]];
        key.seen.fetch_add (1, std::memory_order_relaxed);
        key.formsB1[size_t (forms[k][1])].fetch_add (1, std::memory_order_relaxed);
        key.formsB2[size_t (forms[k][2])].fetch_add (1, std::memory_order_relaxed);
        if (int (k) == root)
            key.wasRoot.fetch_add (1, std::memory_order_relaxed);
        if (int (k) == reference)
            key.wasReference.fetch_add (1, std::memory_order_relaxed);
        if (!poses[k].valid)
            continue;
        key.posed.fetch_add (1, std::memory_order_relaxed);
        key.poseWindow.store (poseWindows[k], std::memory_order_relaxed);

        const bool moved = history.generation + 1 == generation && history.pose.valid &&
                           RotationDifference (poses[k], history.pose) >= kMovingRadians;
        if (moved) {
            key.moving.fetch_add (1, std::memory_order_relaxed);
            Accumulate (key.stepSumDegrees, RotationDifference (poses[k], history.pose) * kDegreesPerRadian);
            if (rootPosed) {
                Tally (Relate (poses[root], g_rootHistory, poses[k], history, generation), key.rootSame,
                       key.rootPrevious, key.rootAhead, key.rootNeither);
                Accumulate (key.rootAngleSumDegrees, RotationDifference (poses[root], poses[k]) * kDegreesPerRadian);
                if (poses[root].hasEye && poses[k].hasEye) {
                    Accumulate (key.rootEyeSum, EyeDistance (poses[root], poses[k]));
                    key.rootEyeSamples.fetch_add (1, std::memory_order_relaxed);
                }
            }
        }
        if (referenceMoved) {
            key.referenceMoving.fetch_add (1, std::memory_order_relaxed);
            Tally (Relate (poses[k], history, poses[reference], g_referenceHistory, generation), key.vsReferenceSame,
                   key.vsReferencePrevious, key.vsReferenceAhead, key.vsReferenceNeither);
        }
    }

    // Keep the raw windows of a few consecutive images for checking by hand.
    const uint32_t dumped = g_dumpCount.load (std::memory_order_relaxed);
    if (readIndex >= kDumpFirstImage && dumped < kDumpImages) {
        DumpImage& image = g_dump[dumped];
        image.generation = generation;
        image.draws = slot.draws;
        for (uint32_t k = 0; k < slot.draws; ++k) {
            const DrawMeta& meta = slot.meta[k];
            DumpDraw& draw = image.draw[k];
            draw.count = meta.count;
            draw.ordinal = meta.ordinal;
            draw.kind = meta.kind;
            draw.root = int (k) == root;
            draw.reference = int (k) == reference;
            draw.vertexShader = meta.vertexShader;
            draw.renderTarget = meta.renderTarget;
            for (size_t s = 0; s < kWindows; ++s) {
                DumpWindow& window = draw.windows[s];
                window.bound = meta.windows[s].IsBound ();
                window.form = uint32_t (forms[k][s]);
                window.buffer = meta.windows[s].buffer;
                window.firstConstant = meta.windows[s].firstConstant;
                window.numConstants = meta.windows[s].numConstants;
                window.bytesCopied = meta.bytesCopied[s];
                std::memcpy (window.values, data[k][s], sizeof (window.values));
            }
        }
        g_dumpCount.store (dumped + 1, std::memory_order_release);
    }

    // Only generation g - 1 is ever compared against, so a skipped or unread
    // image leaves every history one step too old and nothing is misclassified.
    for (uint32_t k = 0; k < slot.draws; ++k) {
        if (keys[k] < 0)
            continue;
        g_keyHistory[keys[k]].generation = generation;
        g_keyHistory[keys[k]].pose = poses[k];
    }
    g_rootHistory.generation = generation;
    g_rootHistory.pose = rootPosed ? poses[root] : Pose {};
    g_referenceHistory.generation = generation;
    g_referenceHistory.pose = referencePosed ? poses[reference] : Pose {};
    g_lastReadGeneration = generation;
    g_lastReadValid = true;
}

} // namespace

void SetEnabled (bool enabled)
{
    g_enabled.store (enabled, std::memory_order_release);
}

bool Enabled ()
{
    return g_enabled.load (std::memory_order_acquire);
}

void Reset ()
{
    for (Slot& slot : g_slots) {
        slot.state.store (uint32_t (SlotState::Free), std::memory_order_relaxed);
        slot.generation = 0;
        slot.sequence = 0;
        slot.draws = 0;
    }
    g_openSlot = -1;
    g_openGeneration = 0;
    g_lastRootCommits = 0;
    g_nextSequence = 0;
    g_lastReadValid = false;
    g_lastReadGeneration = 0;
    for (History& history : g_keyHistory)
        history = History {};
    g_rootHistory = History {};
    g_referenceHistory = History {};
    g_imagesOpened.store (0, std::memory_order_relaxed);
    g_imagesSkipped.store (0, std::memory_order_relaxed);
    g_drawsTruncated.store (0, std::memory_order_relaxed);
    g_createFailures.store (0, std::memory_order_relaxed);
    g_imagesRead.store (0, std::memory_order_relaxed);
    g_imagesConsecutive.store (0, std::memory_order_relaxed);
    g_keysTruncated.store (0, std::memory_order_relaxed);
    g_readbacksPending.store (0, std::memory_order_relaxed);
    g_readbackFailures.store (0, std::memory_order_relaxed);
    g_rootMissing.store (0, std::memory_order_relaxed);
    g_rootPoseMissing.store (0, std::memory_order_relaxed);
    g_referenceMissing.store (0, std::memory_order_relaxed);
    g_imagesMoving.store (0, std::memory_order_relaxed);
    g_rootSame.store (0, std::memory_order_relaxed);
    g_rootPrevious.store (0, std::memory_order_relaxed);
    g_rootAhead.store (0, std::memory_order_relaxed);
    g_rootNeither.store (0, std::memory_order_relaxed);
    g_rootAngleSumDegrees.store (0.0, std::memory_order_relaxed);
    g_referenceStepSumDegrees.store (0.0, std::memory_order_relaxed);
    for (KeyCounters& key : g_keys) {
        key.count.store (0, std::memory_order_relaxed);
        key.ordinal.store (0, std::memory_order_relaxed);
        key.kind.store (0, std::memory_order_relaxed);
        key.seen.store (0, std::memory_order_relaxed);
        key.posed.store (0, std::memory_order_relaxed);
        key.poseWindow.store (-1, std::memory_order_relaxed);
        for (size_t f = 0; f < kFormCount; ++f) {
            key.formsB1[f].store (0, std::memory_order_relaxed);
            key.formsB2[f].store (0, std::memory_order_relaxed);
        }
        key.moving.store (0, std::memory_order_relaxed);
        key.rootSame.store (0, std::memory_order_relaxed);
        key.rootPrevious.store (0, std::memory_order_relaxed);
        key.rootAhead.store (0, std::memory_order_relaxed);
        key.rootNeither.store (0, std::memory_order_relaxed);
        key.referenceMoving.store (0, std::memory_order_relaxed);
        key.vsReferenceSame.store (0, std::memory_order_relaxed);
        key.vsReferencePrevious.store (0, std::memory_order_relaxed);
        key.vsReferenceAhead.store (0, std::memory_order_relaxed);
        key.vsReferenceNeither.store (0, std::memory_order_relaxed);
        key.wasRoot.store (0, std::memory_order_relaxed);
        key.wasReference.store (0, std::memory_order_relaxed);
        key.stepSumDegrees.store (0.0, std::memory_order_relaxed);
        key.rootAngleSumDegrees.store (0.0, std::memory_order_relaxed);
        key.rootEyeSum.store (0.0, std::memory_order_relaxed);
        key.rootEyeSamples.store (0, std::memory_order_relaxed);
    }
    g_keyCount.store (0, std::memory_order_relaxed);
    g_dumpCount.store (0, std::memory_order_relaxed);
}

void OnDrawCompleted (ID3D11DeviceContext* context, uint32_t kind, uint32_t count)
{
    if (!Enabled () || context == nullptr)
        return;
    const uint64_t generation = scenecamerapairing::ContextImageGeneration ();
    const uint64_t rootCommits = scenecamerapairing::ContextRootCommits ();
    const bool isRoot = rootCommits != g_lastRootCommits;
    g_lastRootCommits = rootCommits;
    if (generation == 0)
        return; // no image identified yet -- nothing to compare against

    if (generation != g_openGeneration) {
        SubmitOpenSlot ();
        g_openGeneration = generation;
        OpenSlot (context, generation);
    }
    if (g_openSlot < 0)
        return; // this image was skipped; already counted

    Slot& slot = g_slots[g_openSlot];
    if (slot.draws >= kMaxDraws) {
        g_drawsTruncated.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    const uint32_t k = slot.draws;
    DrawMeta& meta = slot.meta[k];
    meta.count = count;
    meta.kind = kind;
    meta.ordinal = 0;
    for (uint32_t j = 0; j < k; ++j)
        if (slot.meta[j].count == count && slot.meta[j].kind == kind)
            ++meta.ordinal;
    meta.root = isRoot;

    // Every window this draw had bound, whether or not its shader reads it: a
    // stale binding is itself evidence of where a camera came from.
    const contextstate::ContextState live = contextstate::Snapshot ();
    meta.vertexShader = live.vertexShader;
    meta.renderTarget = live.renderTarget;

    // ⚠️ OUR COPIES ARE NOT ARCHICAD'S WORK. Without the guard the hooked copy
    // detour would record these as host operations.
    contextstate::ScopedInjectionGuard guard;
    for (size_t s = 0; s < kWindows; ++s) {
        const contextstate::ConstantBufferBinding& binding = live.vsConstantBuffers[s];
        meta.windows[s] = binding;
        meta.bytesCopied[s] = 0;
        if (!binding.IsBound ())
            continue;
        // The binding is live, so the buffer is too; clamp to it and to the window.
        ID3D11Buffer* source = reinterpret_cast<ID3D11Buffer*> (uintptr_t (binding.buffer));
        D3D11_BUFFER_DESC desc = {};
        source->GetDesc (&desc);
        const uint32_t offset = binding.ByteOffset ();
        if (offset >= desc.ByteWidth)
            continue;
        uint32_t bytes = desc.ByteWidth - offset;
        if (binding.numConstants != 0 && bytes > binding.numConstants * 16u)
            bytes = binding.numConstants * 16u;
        if (bytes > kBytesPerWindow)
            bytes = kBytesPerWindow;
        D3D11_BOX box = {};
        box.left = offset;
        box.right = offset + bytes;
        box.top = 0;
        box.bottom = 1;
        box.front = 0;
        box.back = 1;
        context->CopySubresourceRegion (slot.staging, 0, UINT (k * kBytesPerDraw + s * kBytesPerWindow), 0, 0, source,
                                        0, &box);
        meta.bytesCopied[s] = bytes;
    }
    slot.draws = k + 1;
}

void OnPresent (IDXGISwapChain* swapChain)
{
    if (!Enabled () || swapChain == nullptr)
        return;
    // In submit order: a newer image is never read before an older one, or
    // "previous image" would stop meaning the previous image.
    int oldest = -1;
    for (int i = 0; i < int (kSlots); ++i) {
        if (g_slots[i].state.load (std::memory_order_acquire) != uint32_t (SlotState::Submitted))
            continue;
        if (oldest < 0 || g_slots[i].sequence < g_slots[oldest].sequence)
            oldest = i;
    }
    if (oldest < 0)
        return;
    Slot& slot = g_slots[oldest];

    ImageData data = {};
    if (slot.draws > 0) {
        ID3D11Device* device = nullptr;
        swapChain->GetDevice (__uuidof (ID3D11Device), (void**) &device);
        if (device == nullptr)
            return;
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext (&context);
        device->Release ();
        if (context == nullptr)
            return;
        // Our own Map on Archicad's context is not Archicad's work either.
        contextstate::ScopedInjectionGuard guard;
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT hr = context->Map (slot.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            g_readbacksPending.fetch_add (1, std::memory_order_relaxed);
            context->Release ();
            return;
        }
        if (SUCCEEDED (hr) && mapped.pData != nullptr) {
            std::memcpy (data, mapped.pData, size_t (slot.draws) * kBytesPerDraw);
            context->Unmap (slot.staging, 0);
        }
        context->Release ();
        if (FAILED (hr) || mapped.pData == nullptr) {
            g_readbackFailures.fetch_add (1, std::memory_order_relaxed);
            slot.state.store (uint32_t (SlotState::Free), std::memory_order_release);
            return;
        }
        // The staging ring is reused: bytes this image did not copy are an older
        // image's, so they are cleared before anything reads them.
        for (uint32_t k = 0; k < slot.draws; ++k)
            for (size_t s = 0; s < kWindows; ++s)
                for (size_t f = slot.meta[k].bytesCopied[s] / sizeof (float); f < kFloatsPerWindow; ++f)
                    data[k][s][f] = 0.0f;
    }
    Evaluate (slot, data);
    slot.state.store (uint32_t (SlotState::Free), std::memory_order_release);
}

Stats GetStats ()
{
    Stats stats;
    stats.enabled = Enabled ();
    stats.imagesOpened = g_imagesOpened.load (std::memory_order_relaxed);
    stats.imagesSkipped = g_imagesSkipped.load (std::memory_order_relaxed);
    stats.imagesRead = g_imagesRead.load (std::memory_order_relaxed);
    stats.imagesConsecutive = g_imagesConsecutive.load (std::memory_order_relaxed);
    stats.drawsTruncated = g_drawsTruncated.load (std::memory_order_relaxed);
    stats.keysTruncated = g_keysTruncated.load (std::memory_order_relaxed);
    stats.readbacksPending = g_readbacksPending.load (std::memory_order_relaxed);
    stats.readbackFailures = g_readbackFailures.load (std::memory_order_relaxed);
    stats.createFailures = g_createFailures.load (std::memory_order_relaxed);
    stats.rootMissing = g_rootMissing.load (std::memory_order_relaxed);
    stats.rootPoseMissing = g_rootPoseMissing.load (std::memory_order_relaxed);
    stats.referenceMissing = g_referenceMissing.load (std::memory_order_relaxed);
    stats.imagesMoving = g_imagesMoving.load (std::memory_order_relaxed);
    stats.rootSame = g_rootSame.load (std::memory_order_relaxed);
    stats.rootPrevious = g_rootPrevious.load (std::memory_order_relaxed);
    stats.rootAhead = g_rootAhead.load (std::memory_order_relaxed);
    stats.rootNeither = g_rootNeither.load (std::memory_order_relaxed);
    stats.rootAngleSumDegrees = g_rootAngleSumDegrees.load (std::memory_order_relaxed);
    stats.referenceStepSumDegrees = g_referenceStepSumDegrees.load (std::memory_order_relaxed);
    stats.keyCount = g_keyCount.load (std::memory_order_acquire);
    for (uint32_t j = 0; j < stats.keyCount && j < kMaxKeys; ++j) {
        const KeyCounters& key = g_keys[j];
        KeyStats& out = stats.keys[j];
        out.count = key.count.load (std::memory_order_relaxed);
        out.ordinal = key.ordinal.load (std::memory_order_relaxed);
        out.kind = key.kind.load (std::memory_order_relaxed);
        out.seen = key.seen.load (std::memory_order_relaxed);
        out.posed = key.posed.load (std::memory_order_relaxed);
        out.poseWindow = key.poseWindow.load (std::memory_order_relaxed);
        for (size_t f = 0; f < kFormCount; ++f) {
            out.formsB1[f] = key.formsB1[f].load (std::memory_order_relaxed);
            out.formsB2[f] = key.formsB2[f].load (std::memory_order_relaxed);
        }
        out.moving = key.moving.load (std::memory_order_relaxed);
        out.rootSame = key.rootSame.load (std::memory_order_relaxed);
        out.rootPrevious = key.rootPrevious.load (std::memory_order_relaxed);
        out.rootAhead = key.rootAhead.load (std::memory_order_relaxed);
        out.rootNeither = key.rootNeither.load (std::memory_order_relaxed);
        out.referenceMoving = key.referenceMoving.load (std::memory_order_relaxed);
        out.vsReferenceSame = key.vsReferenceSame.load (std::memory_order_relaxed);
        out.vsReferencePrevious = key.vsReferencePrevious.load (std::memory_order_relaxed);
        out.vsReferenceAhead = key.vsReferenceAhead.load (std::memory_order_relaxed);
        out.vsReferenceNeither = key.vsReferenceNeither.load (std::memory_order_relaxed);
        out.wasRoot = key.wasRoot.load (std::memory_order_relaxed);
        out.wasReference = key.wasReference.load (std::memory_order_relaxed);
        out.stepSumDegrees = key.stepSumDegrees.load (std::memory_order_relaxed);
        out.rootAngleSumDegrees = key.rootAngleSumDegrees.load (std::memory_order_relaxed);
        out.rootEyeSum = key.rootEyeSum.load (std::memory_order_relaxed);
        out.rootEyeSamples = key.rootEyeSamples.load (std::memory_order_relaxed);
    }
    stats.dumpCount = g_dumpCount.load (std::memory_order_acquire);
    return stats;
}

size_t CopyDump (DumpImage* out, size_t capacity)
{
    if (out == nullptr)
        return 0;
    size_t published = g_dumpCount.load (std::memory_order_acquire);
    if (published > capacity)
        published = capacity;
    for (size_t i = 0; i < published; ++i)
        out[i] = g_dump[i];
    return published;
}

} // namespace cameraagreement
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv
