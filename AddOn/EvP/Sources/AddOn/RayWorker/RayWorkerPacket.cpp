#include "RayWorker/RayWorkerPacket.hpp"
#include "Geometry/Mesh.hpp"

#include <cmath>
#include <cstring>
#include <limits>

namespace evp::rayworker::packet {

namespace {

// ⚠️ ARRAYS MOVE AS BLOCKS. The layout is little-endian and so is every
// target this builds for, so an array's bytes ARE its encoding: one memcpy
// each way. Encoding a 40,000-sample request byte by byte cost more than the
// GPU's trace of it (E5, 2026-10-31).
static_assert (sizeof (double) == 8 && sizeof (uint32_t) == 4 && sizeof (uint64_t) == 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "RayWorkerPacket assumes a little-endian target"
#endif

// One packet, written once: the header's 64 bytes are reserved first and
// filled by Seal, so the payload is never copied again.
class Writer {
  public:
    explicit Writer (size_t payloadBytes)
    {
        bytes_.reserve (HeaderBytes + payloadBytes);
        bytes_.resize (HeaderBytes);
    }
    void U32 (uint32_t value)
    {
        Raw (&value, sizeof value);
    }
    void U64 (uint64_t value)
    {
        Raw (&value, sizeof value);
    }
    void F64 (double value)
    {
        Raw (&value, sizeof value);
    }
    template <typename T> void Array (const std::vector<T>& values)
    {
        static_assert (sizeof (T) == 1 || sizeof (T) == 4 || sizeof (T) == 8);
        if (!values.empty ())
            Raw (values.data (), values.size () * sizeof (T));
    }
    std::vector<uint8_t>& Bytes ()
    {
        return bytes_;
    }

  private:
    void Raw (const void* data, size_t size)
    {
        const size_t at = bytes_.size ();
        bytes_.resize (at + size);
        std::memcpy (bytes_.data () + at, data, size);
    }

    std::vector<uint8_t> bytes_;
};

class Reader {
  public:
    Reader (const uint8_t* bytes, size_t size, std::string& error) : bytes_ (bytes), size_ (size), error_ (error)
    {
    }
    uint32_t U32 ()
    {
        uint32_t value = 0;
        Raw (&value, sizeof value);
        return value;
    }
    uint64_t U64 ()
    {
        uint64_t value = 0;
        Raw (&value, sizeof value);
        return value;
    }
    double F64 ()
    {
        double value = 0.0;
        Raw (&value, sizeof value);
        return value;
    }
    // A count of `width`-byte elements, refused before allocation when the
    // bytes left cannot hold it.
    bool Count (uint64_t count, size_t width, const char* what)
    {
        if (!ok_)
            return false;
        if (count > (size_ - at_) / width) {
            Fail (std::string (what) + " claimed " + std::to_string (count) + " entries, more than the segment holds");
            return false;
        }
        return true;
    }
    template <typename T> bool Array (uint64_t count, std::vector<T>& out, const char* what)
    {
        if (!Count (count, sizeof (T), what))
            return false;
        out.resize (static_cast<size_t> (count));
        Raw (out.data (), out.size () * sizeof (T));
        return ok_;
    }
    bool F64s (uint64_t count, std::vector<double>& out, const char* what)
    {
        if (!Array (count, out, what))
            return false;
        for (const double value : out)
            if (!std::isfinite (value)) {
                Fail (std::string (what) + " contains a non-finite value");
                return false;
            }
        return true;
    }
    bool Finish (const char* what)
    {
        if (ok_ && at_ != size_)
            Fail (std::string (what) + " has " + std::to_string (size_ - at_) + " trailing byte(s)");
        return ok_;
    }
    void Fail (const std::string& message)
    {
        if (ok_)
            error_ = message;
        ok_ = false;
    }
    bool Ok () const
    {
        return ok_;
    }

  private:
    void Raw (void* out, size_t count)
    {
        if (!ok_ || count == 0)
            return;
        if (size_ - at_ < count) {
            Fail ("the segment is short");
            return;
        }
        std::memcpy (out, bytes_ + at_, count);
        at_ += count;
    }

    const uint8_t* bytes_;
    size_t size_;
    size_t at_ = 0;
    bool ok_ = true;
    std::string& error_;
};

void PutU32 (uint8_t* at, uint32_t value)
{
    std::memcpy (at, &value, sizeof value);
}

void PutU64 (uint8_t* at, uint64_t value)
{
    std::memcpy (at, &value, sizeof value);
}

// Header: magic, layout, kind, reserved (4 x u32), epoch, sequence,
// payloadBytes, checksum (4 x u64), 16 reserved bytes -> 64.
std::vector<uint8_t> Seal (Kind kind, uint64_t epoch, uint64_t sequence, Writer& writer)
{
    std::vector<uint8_t> out = std::move (writer.Bytes ());
    const size_t payload = out.size () - HeaderBytes;
    std::memset (out.data (), 0, HeaderBytes);
    PutU32 (out.data (), Magic);
    PutU32 (out.data () + 4, LayoutVersion);
    PutU32 (out.data () + 8, static_cast<uint32_t> (kind));
    PutU64 (out.data () + 16, epoch);
    PutU64 (out.data () + 24, sequence);
    PutU64 (out.data () + 32, payload);
    PutU64 (out.data () + 40, Checksum (out.data () + HeaderBytes, payload));
    return out;
}

// Validate the header; on success `payload` and `payloadSize` describe the
// checked payload.
bool Open (const uint8_t* bytes, size_t size, Kind kind, uint64_t epoch, const uint64_t* sequence, uint64_t checksum,
           const uint8_t*& payload, size_t& payloadSize, std::string& error)
{
    if (bytes == nullptr || size < HeaderBytes) {
        error = "the segment is shorter than a packet header";
        return false;
    }
    Reader r (bytes, HeaderBytes, error);
    if (r.U32 () != Magic) {
        error = "the segment does not hold a ray-worker packet";
        return false;
    }
    const uint32_t layout = r.U32 ();
    if (layout != LayoutVersion) {
        error =
            "the packet layout version is " + std::to_string (layout) + ", expected " + std::to_string (LayoutVersion);
        return false;
    }
    if (r.U32 () != static_cast<uint32_t> (kind)) {
        error = "the segment holds another kind of packet";
        return false;
    }
    r.U32 ();
    const uint64_t storedEpoch = r.U64 ();
    const uint64_t storedSequence = r.U64 ();
    const uint64_t payloadBytes = r.U64 ();
    const uint64_t storedChecksum = r.U64 ();
    if (storedEpoch != epoch) {
        error = "the packet belongs to scene epoch " + std::to_string (storedEpoch) + ", not " + std::to_string (epoch);
        return false;
    }
    if (sequence != nullptr && storedSequence != *sequence) {
        error = "the packet belongs to run " + std::to_string (storedSequence) + ", not " + std::to_string (*sequence);
        return false;
    }
    if (payloadBytes != size - HeaderBytes) {
        error = "the packet declares " + std::to_string (payloadBytes) + " payload bytes but the segment holds " +
                std::to_string (size - HeaderBytes);
        return false;
    }
    payload = bytes + HeaderBytes;
    payloadSize = static_cast<size_t> (payloadBytes);
    if (storedChecksum != checksum || Checksum (payload, payloadSize) != checksum) {
        error = "the packet checksum does not match: the segment is stale or partially written";
        return false;
    }
    return true;
}

} // namespace

uint64_t Checksum (const uint8_t* bytes, size_t size)
{
    // Four independent FNV-1a lanes over 64-bit words, folded with the length
    // and the tail bytes. A byte-wise FNV-1a is one dependent multiply per
    // byte; this is one per 8 bytes, four in flight. It detects a stale or
    // partial segment, which is all it is for -- not an adversary.
    constexpr uint64_t prime = 1099511628211ull;
    uint64_t lane[4] = { 1469598103934665603ull, 0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull };
    size_t at = 0;
    for (; at + 32 <= size; at += 32)
        for (int k = 0; k < 4; ++k) {
            uint64_t word = 0;
            std::memcpy (&word, bytes + at + 8 * k, 8);
            lane[k] = (lane[k] ^ word) * prime;
        }
    uint64_t hash = 1469598103934665603ull ^ static_cast<uint64_t> (size);
    for (const uint64_t value : lane)
        hash = (hash ^ value) * prime;
    for (; at < size; ++at)
        hash = (hash ^ bytes[at]) * prime;
    return hash;
}

Scene SceneFromSnapshot (const geomsrv::Snapshot& snapshot)
{
    Scene scene;
    scene.meshCount = static_cast<uint32_t> (snapshot.meshes.size ());
    for (size_t m = 0; m < snapshot.meshes.size (); ++m) {
        const geomsrv::Mesh& mesh = snapshot.meshes[m];
        const uint32_t base = static_cast<uint32_t> (scene.vertices.size () / 3);
        scene.vertices.insert (scene.vertices.end (), mesh.vertices.begin (), mesh.vertices.end ());
        for (const uint32_t index : mesh.triangles)
            scene.triangles.push_back (base + index);
        scene.triangleMesh.resize (scene.triangles.size () / 3, static_cast<uint32_t> (m));
    }
    return scene;
}

std::vector<uint8_t> Write (uint64_t epoch, const Scene& scene)
{
    Writer w (20 + scene.vertices.size () * 8 + (scene.triangles.size () + scene.triangleMesh.size ()) * 4);
    w.U32 (scene.meshCount);
    w.U64 (scene.vertices.size () / 3);
    w.U64 (scene.triangles.size () / 3);
    w.Array (scene.vertices);
    w.Array (scene.triangles);
    w.Array (scene.triangleMesh);
    return Seal (Kind::Scene, epoch, 0, w);
}

std::vector<uint8_t> Write (uint64_t epoch, uint64_t sequence, const Request& request)
{
    Writer w (44 + (request.positions.size () + request.normals.size () + request.directions.size ()) * 8 +
              request.steps.size () * 4);
    w.F64 (request.tmin);
    w.F64 (request.tmax);
    w.U64 (request.totalSteps);
    w.U64 (request.positions.size () / 3);
    w.U32 (request.normals.empty () ? 0u : 1u);
    w.U64 (request.steps.size ());
    w.Array (request.positions);
    w.Array (request.normals);
    w.Array (request.steps);
    w.Array (request.directions);
    return Seal (Kind::Request, epoch, sequence, w);
}

std::vector<uint8_t> Write (uint64_t epoch, uint64_t sequence, const Result& result)
{
    Writer w (16 + result.bits.size () * 8 + result.resolved.size ());
    w.U64 (result.sampleCount);
    w.U64 (result.stepCount);
    w.Array (result.bits);
    w.Array (result.resolved);
    return Seal (Kind::Result, epoch, sequence, w);
}

uint64_t StoredChecksum (const std::vector<uint8_t>& packet)
{
    if (packet.size () < HeaderBytes)
        return 0;
    uint64_t value = 0;
    std::memcpy (&value, packet.data () + 40, sizeof value);
    return value;
}

bool Read (const uint8_t* bytes, size_t size, uint64_t epoch, uint64_t checksum, Scene& scene, std::string& error)
{
    const uint8_t* payload = nullptr;
    size_t payloadSize = 0;
    if (!Open (bytes, size, Kind::Scene, epoch, nullptr, checksum, payload, payloadSize, error))
        return false;
    Reader r (payload, payloadSize, error);
    Scene out;
    out.meshCount = r.U32 ();
    const uint64_t vertexCount = r.U64 ();
    const uint64_t triangleCount = r.U64 ();
    if (vertexCount > std::numeric_limits<uint32_t>::max () || triangleCount > std::numeric_limits<uint32_t>::max ())
        r.Fail ("the scene is larger than 32-bit indices can address");
    r.F64s (vertexCount * 3, out.vertices, "the vertex array");
    r.Array (triangleCount * 3, out.triangles, "the triangle array");
    r.Array (triangleCount, out.triangleMesh, "the triangle-mesh array");
    if (!r.Finish ("the scene"))
        return false;
    for (const uint32_t index : out.triangles)
        if (index >= vertexCount) {
            error = "a triangle names vertex " + std::to_string (index) + " of " + std::to_string (vertexCount);
            return false;
        }
    for (const uint32_t mesh : out.triangleMesh)
        if (mesh >= out.meshCount) {
            error = "a triangle names mesh " + std::to_string (mesh) + " of " + std::to_string (out.meshCount);
            return false;
        }
    scene = std::move (out);
    return true;
}

bool Read (const uint8_t* bytes, size_t size, uint64_t epoch, uint64_t sequence, uint64_t checksum, Request& request,
           std::string& error)
{
    const uint8_t* payload = nullptr;
    size_t payloadSize = 0;
    if (!Open (bytes, size, Kind::Request, epoch, &sequence, checksum, payload, payloadSize, error))
        return false;
    Reader r (payload, payloadSize, error);
    Request out;
    out.tmin = r.F64 ();
    out.tmax = r.F64 ();
    out.totalSteps = r.U64 ();
    const uint64_t samples = r.U64 ();
    const uint32_t hasNormals = r.U32 ();
    const uint64_t steps = r.U64 ();
    if (r.Ok () && (hasNormals > 1 || !std::isfinite (out.tmin) || !std::isfinite (out.tmax)))
        r.Fail ("the request settings are malformed");
    if (r.Ok () && samples > payloadSize / 24)
        r.Fail ("the request claims more samples than the segment holds");
    r.F64s (samples * 3, out.positions, "the sample positions");
    r.F64s (hasNormals != 0 ? samples * 3 : 0, out.normals, "the sample normals");
    r.Array (steps, out.steps, "the step list");
    r.F64s (steps * 3, out.directions, "the sun directions");
    if (!r.Finish ("the request"))
        return false;
    for (size_t i = 0; i < out.steps.size (); ++i)
        if (out.steps[i] >= out.totalSteps || (i > 0 && out.steps[i] <= out.steps[i - 1])) {
            error = "the step list is out of range or not strictly ascending";
            return false;
        }
    request = std::move (out);
    return true;
}

bool Read (const uint8_t* bytes, size_t size, uint64_t epoch, uint64_t sequence, uint64_t checksum, Result& result,
           std::string& error)
{
    const uint8_t* payload = nullptr;
    size_t payloadSize = 0;
    if (!Open (bytes, size, Kind::Result, epoch, &sequence, checksum, payload, payloadSize, error))
        return false;
    Reader r (payload, payloadSize, error);
    Result out;
    out.sampleCount = r.U64 ();
    out.stepCount = r.U64 ();
    const uint64_t words = (out.stepCount + 63) / 64;
    if (r.Ok () && words != 0 && out.sampleCount > payloadSize / (8 * words))
        r.Fail ("the result claims more samples than the segment holds");
    r.Array (out.sampleCount * words, out.bits, "the lit bitset");
    r.Array (out.stepCount, out.resolved, "the resolved-step flags");
    if (!r.Finish ("the result"))
        return false;
    result = std::move (out);
    return true;
}

} // namespace evp::rayworker::packet
