#ifndef TAPIOCA_GEOMETRY_EXTRACTION_CAPTURE_HPP
#define TAPIOCA_GEOMETRY_EXTRACTION_CAPTURE_HPP

// What reached GeometryExtractor's SDK-free seam, recorded so a real model can be replayed
// offline against the C++ oracle (AppendExtractedConvex) and, later, the Rust composition.
// Nothing is recorded unless a ScopedExtractionRecording armed a recorder on this thread;
// production extraction neither reads nor depends on it.
//
// Recorded per element: each body that passed the extractor's vertex/polygon guard with its
// full world-space vertex list; each polygon whose edges were read, with its sorted visible
// edge keys, material and surface attribute; each convex polygon exactly as handed to
// AppendExtractedConvex (raw SDK corner normals, before normalization). Polygons the SDK
// threw on, and convexes skipped for bad indices or fewer than three corners, never reach
// the seam and are absent.

#include "MeshExtraction.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace geomsrv {

struct CapturedPolygon {
    int32_t index = 0, material = 0, surface = 0;
    std::vector<WireEdgeKey> visibleEdges;
    std::vector<std::vector<ExtractionCorner>> convexes;
};
struct CapturedBody {
    int32_t index = 0;
    std::vector<double> positions; // xyz, body-local vertex order
    std::vector<CapturedPolygon> polygons;
};
struct CapturedElement {
    std::string guid;
    int32_t elemType = 0;
    std::vector<CapturedBody> bodies;
};
struct ExtractionCapture {
    std::vector<CapturedElement> elements;
};

class ExtractionRecorder {
  public:
    // A recording past `maxValues` doubles (positions + corner normals) stops growing and
    // says so, rather than taking the session's memory.
    explicit ExtractionRecorder (size_t maxValues = size_t (32) << 20);
    void BeginElement (const std::string& guid, int32_t elemType);
    void BeginBody (int32_t index, std::span<const double> positions);
    void BeginPolygon (int32_t index, int32_t material, int32_t surface, const std::vector<WireEdgeKey>& visibleEdges);
    void Convex (const std::vector<ExtractionCorner>& corners);
    const ExtractionCapture& Capture () const
    {
        return capture;
    }
    bool Truncated () const
    {
        return truncated;
    }

  private:
    bool Take (size_t values);
    ExtractionCapture capture;
    size_t maxValues, values = 0;
    bool truncated = false;
};

// The recorder ExtractElement writes to: null unless a scope armed one on this thread.
ExtractionRecorder* ActiveExtractionRecorder ();
class ScopedExtractionRecording {
  public:
    explicit ScopedExtractionRecording (ExtractionRecorder& recorder);
    ~ScopedExtractionRecording ();
    ScopedExtractionRecording (const ScopedExtractionRecording&) = delete;
    ScopedExtractionRecording& operator= (const ScopedExtractionRecording&) = delete;

  private:
    ExtractionRecorder* previous;
};

// One element through the production seam, as ExtractElement drives it: bounds over every
// recorded body vertex, the welder reset per body, the smooth-normal safety net last.
struct ExtractionReplay {
    Mesh mesh;
    ExtractionCounts counts;
    bool retained = false;
};
ExtractionReplay ReplayExtraction (const CapturedElement& element);

// Versioned little-endian binary; doubles keep their exact bits. Reading is strict: bad
// magic or version, counts the remaining bytes cannot hold, and trailing bytes all refuse
// and leave `out` untouched.
std::string WriteExtractionCapture (const ExtractionCapture& capture);
bool ReadExtractionCapture (std::string_view bytes, ExtractionCapture& out, std::string& error);

} // namespace geomsrv
#endif
