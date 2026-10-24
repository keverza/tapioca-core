#include "RayWorker/RayWorkerPacket.hpp"
#include "Geometry/Mesh.hpp"

#include <cmath>
#include <cstring>
#include <limits>

namespace evp::rayworker::packet {

namespace {

class Writer {
  public:
    void U32 (uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
            bytes_.push_back (static_cast<uint8_t> ((value >> shift) & 0xFFu));
    }
    void U64 (uint64_t value)
    {
        for (int shift = 0; shift < 64; shift += 8)
            bytes_.push_back (static_cast<uint8_t> ((value >> shift) & 0xFFu));
    }
    void F64 (double value)
    {
        uint64_t bits = 0;
        std::memcpy (&bits, &value, sizeof bits);
        U64 (bits);
    }
    template <typename T, typename F> void Array (const std::vector<T>& values, F write)
    {
        for (const T& value : values)
            (this->*write) (value);
    }
    std::vector<uint8_t>& Bytes ()
    {
        return bytes_;
    }

  private:
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
        if (Need (4))
            for (int i = 0; i < 4; ++i)
                value |= static_cast<uint32_t> (bytes_[at_++]) << (8 * i);
        return value;
    }
    uint64_t U64 ()
    {
        uint64_t value = 0;
        if (Need (8))
            for (int i = 0; i < 8; ++i)
                value |= static_cast<uint64_t> (bytes_[at_++]) << (8 * i);
        return value;
    }
    double F64 ()
    {
        const uint64_t bits = U64 ();
        double value = 0.0;
        std::memcpy (&value, &bits, sizeof value);
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
    template <typename T> bool U32s (uint64_t count, std::vector<T>& out, const char* what)
    {
        if (!Count (count, 4, what))
            return false;
        out.resize (static_cast<size_t> (count));
        for (T& value : out)
            value = static_cast<T> (U32 ());
        return ok_;
    }
    bool U64s (uint64_t count, std::vector<uint64_t>& out, const char* what)
    {
        if (!Count (count, 8, what))
            return false;
        out.resize (static_cast<size_t> (count));
        for (uint64_t& value : out)
            value = U64 ();
        return ok_;
    }
    bool F64s (uint64_t count, std::vector<double>& out, const char* what)
    {
        if (!Count (count, 8, what))
            return false;
        out.resize (static_cast<size_t> (count));
        for (double& value : out) {
            value = F64 ();
            if (!std::isfinite (value)) {
                Fail (std::string (what) + " contains a non-finite value");
                return false;
            }
        }
        return ok_;
    }
    bool Bytes (uint64_t count, std::vector<uint8_t>& out, const char* what)
    {
        if (!Count (count, 1, what))
            return false;
        out.assign (bytes_ + at_, bytes_ + at_ + count);
        at_ += static_cast<size_t> (count);
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
    bool Need (size_t count)
    {
        if (!ok_)
            return false;
        if (size_ - at_ < count) {
            Fail ("the segment is short");
            return false;
        }
        return true;
    }

    const uint8_t* bytes_;
    size_t size_;
    size_t at_ = 0;
    bool ok_ = true;
    std::string& error_;
};

// Header: magic, layout, kind, reserved (4 x u32), epoch, sequence,
// payloadBytes, checksum (4 x u64), 16 reserved bytes -> 64.
std::vector<uint8_t> Seal (Kind kind, uint64_t epoch, uint64_t sequence, std::vector<uint8_t> payload)
{
    Writer header;
    header.U32 (Magic);
    header.U32 (LayoutVersion);
    header.U32 (static_cast<uint32_t> (kind));
    header.U32 (0);
    header.U64 (epoch);
    header.U64 (sequence);
    header.U64 (payload.size ());
    header.U64 (Checksum (payload.data (), payload.size ()));
    header.U64 (0);
    header.U64 (0);
    std::vector<uint8_t> out = std::move (header.Bytes ());
    out.insert (out.end (), payload.begin (), payload.end ());
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
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
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
    Writer w;
    w.U32 (scene.meshCount);
    w.U64 (scene.vertices.size () / 3);
    w.U64 (scene.triangles.size () / 3);
    w.Array (scene.vertices, &Writer::F64);
    w.Array (scene.triangles, &Writer::U32);
    w.Array (scene.triangleMesh, &Writer::U32);
    return Seal (Kind::Scene, epoch, 0, std::move (w.Bytes ()));
}

std::vector<uint8_t> Write (uint64_t epoch, uint64_t sequence, const Request& request)
{
    Writer w;
    w.F64 (request.tmin);
    w.F64 (request.tmax);
    w.U64 (request.totalSteps);
    w.U64 (request.positions.size () / 3);
    w.U32 (request.normals.empty () ? 0u : 1u);
    w.U64 (request.steps.size ());
    w.Array (request.positions, &Writer::F64);
    w.Array (request.normals, &Writer::F64);
    w.Array (request.steps, &Writer::U32);
    w.Array (request.directions, &Writer::F64);
    return Seal (Kind::Request, epoch, sequence, std::move (w.Bytes ()));
}

std::vector<uint8_t> Write (uint64_t epoch, uint64_t sequence, const Result& result)
{
    Writer w;
    w.U64 (result.sampleCount);
    w.U64 (result.stepCount);
    w.Array (result.bits, &Writer::U64);
    for (const uint8_t flag : result.resolved)
        w.Bytes ().push_back (flag);
    return Seal (Kind::Result, epoch, sequence, std::move (w.Bytes ()));
}

uint64_t StoredChecksum (const std::vector<uint8_t>& packet)
{
    if (packet.size () < HeaderBytes)
        return 0;
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
        value |= static_cast<uint64_t> (packet[40 + i]) << (8 * i);
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
    r.U32s (triangleCount * 3, out.triangles, "the triangle array");
    r.U32s (triangleCount, out.triangleMesh, "the triangle-mesh array");
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
    r.U32s (steps, out.steps, "the step list");
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
    r.U64s (out.sampleCount * words, out.bits, "the lit bitset");
    r.Bytes (out.stepCount, out.resolved, "the resolved-step flags");
    if (!r.Finish ("the result"))
        return false;
    result = std::move (out);
    return true;
}

} // namespace evp::rayworker::packet
