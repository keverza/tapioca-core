#ifndef EVP_ARCHVIZ_SCENETEXTLAYOUTCACHE_HPP
#define EVP_ARCHVIZ_SCENETEXTLAYOUTCACHE_HPP

#include "ArchViz/SceneTextLayout.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace geomsrv::archviz {

struct SceneTextLayoutCacheStats {
    bool running = false;
    size_t entries = 0;
    size_t pending = 0;
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t rejected = 0;
    uint64_t failures = 0;
};

// A viewport-owned shaping worker. Requests never wait on the render thread;
// completed runs are immutable and survive cache eviction through shared ownership.
class SceneTextLayoutCache final {
  public:
    explicit SceneTextLayoutCache (size_t capacity = 512);
    ~SceneTextLayoutCache ();
    SceneTextLayoutCache (const SceneTextLayoutCache&) = delete;
    SceneTextLayoutCache& operator= (const SceneTextLayoutCache&) = delete;

    bool Start (const uint8_t* fontBytes, size_t fontByteCount, std::string& error);
    void Stop ();

    std::shared_ptr<const SceneTextGlyphRun> FindOrRequest (const std::string& utf8,
                                                            SceneTextDirection direction = SceneTextDirection::Auto);
    std::shared_ptr<const SceneTextGlyphRun> WaitFor (const std::string& utf8, SceneTextDirection direction,
                                                      std::string& error);
    SceneTextLayoutCacheStats Stats () const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace geomsrv::archviz

#endif
