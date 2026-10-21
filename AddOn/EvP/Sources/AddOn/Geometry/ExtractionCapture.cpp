#include "ExtractionCapture.hpp"

#include <bit>
#include <cstring>

namespace geomsrv {

namespace {

thread_local ExtractionRecorder* active = nullptr;

constexpr char kMagic[8] = { 'T', 'P', 'X', 'C', 'A', 'P', '0', '1' };
constexpr uint32_t kVersion = 1;

static_assert (std::endian::native == std::endian::little, "the capture format is little-endian");

class Writer {
  public:
    template <class T> void Put (T value)
    {
        char bytes[sizeof (T)];
        std::memcpy (bytes, &value, sizeof (T));
        out.append (bytes, sizeof (T));
    }
    void Count (size_t n)
    {
        Put (uint32_t (n));
    }
    std::string out;
};

class Reader {
  public:
    explicit Reader (std::string_view bytes) : bytes (bytes)
    {
    }
    template <class T> bool Get (T& value)
    {
        if (bytes.size () - at < sizeof (T))
            return false;
        std::memcpy (&value, bytes.data () + at, sizeof (T));
        at += sizeof (T);
        return true;
    }
    // A count whose items, each at least `itemBytes` long, fit in what is left: a forged
    // count cannot make the reader allocate more than the file could describe.
    bool Count (size_t itemBytes, size_t& n)
    {
        uint32_t value = 0;
        if (!Get (value))
            return false;
        n = value;
        return itemBytes == 0 || n <= (bytes.size () - at) / itemBytes;
    }
    bool Text (std::string& text)
    {
        size_t n = 0;
        if (!Count (1, n))
            return false;
        text.assign (bytes.data () + at, n);
        at += n;
        return true;
    }
    bool Done () const
    {
        return at == bytes.size ();
    }

  private:
    std::string_view bytes;
    size_t at = 0;
};

} // namespace

ExtractionRecorder::ExtractionRecorder (size_t maxValues) : maxValues (maxValues)
{
}

bool ExtractionRecorder::Take (size_t n)
{
    if (truncated || n > maxValues - values) {
        truncated = true;
        return false;
    }
    values += n;
    return true;
}

void ExtractionRecorder::BeginElement (const std::string& guid, int32_t elemType)
{
    if (truncated)
        return;
    capture.elements.push_back ({ guid, elemType, {} });
}

void ExtractionRecorder::BeginBody (int32_t index, std::span<const double> positions)
{
    if (capture.elements.empty () || !Take (positions.size ()))
        return;
    capture.elements.back ().bodies.push_back ({ index, { positions.begin (), positions.end () }, {} });
}

void ExtractionRecorder::BeginPolygon (int32_t index, int32_t material, int32_t surface,
                                       const std::vector<WireEdgeKey>& visibleEdges)
{
    if (truncated || capture.elements.empty () || capture.elements.back ().bodies.empty ())
        return;
    capture.elements.back ().bodies.back ().polygons.push_back ({ index, material, surface, visibleEdges, {} });
}

void ExtractionRecorder::Convex (const std::vector<ExtractionCorner>& corners)
{
    if (capture.elements.empty () || capture.elements.back ().bodies.empty () ||
        capture.elements.back ().bodies.back ().polygons.empty () || !Take (corners.size () * 3))
        return;
    capture.elements.back ().bodies.back ().polygons.back ().convexes.push_back (corners);
}

ExtractionRecorder* ActiveExtractionRecorder ()
{
    return active;
}

ScopedExtractionRecording::ScopedExtractionRecording (ExtractionRecorder& recorder) : previous (active)
{
    active = &recorder;
}

ScopedExtractionRecording::~ScopedExtractionRecording ()
{
    active = previous;
}

ExtractionReplay ReplayExtraction (const CapturedElement& element)
{
    ExtractionReplay result;
    auto& mesh = result.mesh;
    mesh.guid = element.guid;
    mesh.elemType = element.elemType;
    VertexWelder welder (mesh);
    std::vector<ExtractionCorner> corners;
    for (const auto& body : element.bodies) {
        for (size_t i = 0; i + 2 < body.positions.size (); i += 3)
            mesh.bounds.Expand (body.positions[i], body.positions[i + 1], body.positions[i + 2]);
        welder.Reset ();
        for (const auto& polygon : body.polygons)
            for (const auto& convex : polygon.convexes) {
                corners = convex; // the seam normalizes its scratch in place
                AppendExtractedConvex (
                    mesh, welder, body.positions, corners,
                    { body.index, polygon.index, polygon.material, polygon.surface, polygon.visibleEdges },
                    result.counts);
            }
    }
    result.retained = mesh.TriangleCount () != 0;
    if (result.retained && !NormalsAreUsable (mesh))
        ComputeSmoothNormals (mesh);
    return result;
}

std::string WriteExtractionCapture (const ExtractionCapture& capture)
{
    Writer w;
    w.out.append (kMagic, sizeof kMagic);
    w.Put (kVersion);
    w.Count (capture.elements.size ());
    for (const auto& element : capture.elements) {
        w.Count (element.guid.size ());
        w.out += element.guid;
        w.Put (element.elemType);
        w.Count (element.bodies.size ());
        for (const auto& body : element.bodies) {
            w.Put (body.index);
            w.Count (body.positions.size ());
            for (double v : body.positions)
                w.Put (v);
            w.Count (body.polygons.size ());
            for (const auto& polygon : body.polygons) {
                w.Put (polygon.index);
                w.Put (polygon.material);
                w.Put (polygon.surface);
                w.Count (polygon.visibleEdges.size ());
                for (WireEdgeKey key : polygon.visibleEdges)
                    w.Put (key);
                w.Count (polygon.convexes.size ());
                for (const auto& convex : polygon.convexes) {
                    w.Count (convex.size ());
                    for (const auto& corner : convex) {
                        w.Put (corner.vertex);
                        w.Put (corner.nx);
                        w.Put (corner.ny);
                        w.Put (corner.nz);
                    }
                }
            }
        }
    }
    return std::move (w.out);
}

bool ReadExtractionCapture (std::string_view bytes, ExtractionCapture& out, std::string& error)
{
    const auto refuse = [&] (const char* why) {
        error = std::string ("extraction capture: ") + why;
        return false;
    };
    if (bytes.size () < sizeof kMagic || std::memcmp (bytes.data (), kMagic, sizeof kMagic) != 0)
        return refuse ("not a capture file");
    Reader r (bytes.substr (sizeof kMagic));
    uint32_t version = 0;
    if (!r.Get (version) || version != kVersion)
        return refuse ("unsupported version");
    // Smallest encodings: element 12 bytes, body 12, polygon 20, convex 4, corner 28.
    ExtractionCapture capture;
    size_t elements = 0;
    if (!r.Count (12, elements))
        return refuse ("truncated element count");
    capture.elements.resize (elements);
    for (auto& element : capture.elements) {
        size_t bodies = 0;
        if (!r.Text (element.guid) || !r.Get (element.elemType) || !r.Count (12, bodies))
            return refuse ("truncated element");
        element.bodies.resize (bodies);
        for (auto& body : element.bodies) {
            size_t positions = 0, polygons = 0;
            if (!r.Get (body.index) || !r.Count (sizeof (double), positions) || positions % 3 != 0)
                return refuse ("truncated or malformed body");
            body.positions.resize (positions);
            for (double& v : body.positions)
                if (!r.Get (v))
                    return refuse ("truncated positions");
            if (!r.Count (20, polygons))
                return refuse ("truncated polygon count");
            body.polygons.resize (polygons);
            for (auto& polygon : body.polygons) {
                size_t edges = 0, convexes = 0;
                if (!r.Get (polygon.index) || !r.Get (polygon.material) || !r.Get (polygon.surface) ||
                    !r.Count (sizeof (WireEdgeKey), edges))
                    return refuse ("truncated polygon");
                polygon.visibleEdges.resize (edges);
                for (auto& key : polygon.visibleEdges)
                    if (!r.Get (key))
                        return refuse ("truncated edges");
                if (!r.Count (4, convexes))
                    return refuse ("truncated convex count");
                polygon.convexes.resize (convexes);
                for (auto& convex : polygon.convexes) {
                    size_t corners = 0;
                    if (!r.Count (28, corners))
                        return refuse ("truncated corner count");
                    convex.resize (corners);
                    for (auto& corner : convex)
                        if (!r.Get (corner.vertex) || !r.Get (corner.nx) || !r.Get (corner.ny) || !r.Get (corner.nz))
                            return refuse ("truncated corner");
                }
            }
        }
    }
    if (!r.Done ())
        return refuse ("trailing bytes");
    out = std::move (capture);
    error.clear ();
    return true;
}

} // namespace geomsrv
