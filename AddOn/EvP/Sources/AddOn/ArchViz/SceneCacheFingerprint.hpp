#ifndef EVP_ARCHVIZ_SCENECACHEFINGERPRINT_HPP
#define EVP_ARCHVIZ_SCENECACHEFINGERPRINT_HPP

// ArchViz/SceneCacheFingerprint -- the scene cache's readings of the live model (#14).
//
// MAIN THREAD, all of it: the ModelerAPI element and the project are DevKit objects. The rule they
// feed is pure (ProjectSceneCache.hpp); this file only reads.
//
// ⚠️ NOTHING HERE TESSELLATES OR WALKS A POLYGON'S CORNERS. The cheap tier is the content sweep's
// own reading (ModelContentWatch.cpp: ~5.4 ms over 3889 elements); the deep tier reads the body
// vertices and each polygon's surface, which is a fraction of what ExtractElementAt does with the
// same element (normals, edges, convex decomposition, texture coordinates, material facts).

#include "ArchViz/ProjectSceneCache.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace ModelerAPI {
class Element;
}

namespace geomsrv {
namespace archviz {
namespace scenecache {

struct CheapReading {
    std::string guid; // empty for an element with no GUID of its own: never cached
    uint32_t genId = 0;
    uint64_t shape = 0;
};

// MAIN THREAD.
CheapReading ReadCheap (const ModelerAPI::Element& element);
// MAIN THREAD. `pool`: the live pool's identities, so a polygon's surface hashes by WHAT it is.
uint64_t ReadDeep (const ModelerAPI::Element& element, const std::map<int32_t, CapturedMaterialIdentity>& pool);

// MAIN THREAD. "solo:<file path, lower case>" or "team:<project path>/<project name>"; empty for an
// untitled project or a failed read, and then there is no cache.
std::string ReadProjectIdentity ();

} // namespace scenecache
} // namespace archviz
} // namespace geomsrv

#endif
