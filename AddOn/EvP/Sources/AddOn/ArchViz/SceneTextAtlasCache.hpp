#ifndef EVP_ARCHVIZ_SCENETEXTATLASCACHE_HPP
#define EVP_ARCHVIZ_SCENETEXTATLASCACHE_HPP

#include "ArchViz/SceneTextAtlas.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv::archviz {

struct SceneTextAtlasCacheStats {
    bool running = false;
    size_t pending = 0;
    size_t stagingPages = 0;
    size_t stagingBytes = 0;
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t rejected = 0;
    uint64_t failures = 0;
    uint64_t generatedPages = 0;
    uint64_t generatedGlyphs = 0;
    uint64_t generationMicroseconds = 0;
};

// A viewport-owned CPU atlas worker. Requests and completed-page retrieval do
// not wait for glyph generation or GPU work.
class SceneTextAtlasCache final {
  public:
    static constexpr size_t kMaximumPendingGlyphs = 256;
    static constexpr size_t kMaximumStagingPages = 2;

    SceneTextAtlasCache ();
    ~SceneTextAtlasCache ();
    SceneTextAtlasCache (const SceneTextAtlasCache&) = delete;
    SceneTextAtlasCache& operator= (const SceneTextAtlasCache&) = delete;

    bool Start (const uint8_t* fontBytes, size_t fontByteCount, std::string& error);
    void Stop ();
    bool Request (const std::vector<uint32_t>& glyphIds);
    std::shared_ptr<const SceneTextAtlasPage> TakeReady ();
    SceneTextAtlasCacheStats Stats () const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace geomsrv::archviz

#endif
