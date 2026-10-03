#ifndef EVP_SUNSTUDY_ATLASREUSE_HPP
#define EVP_SUNSTUDY_ATLASREUSE_HPP

#include "Geometry/Mesh.hpp"
#include "SunStudy/SunStudyAtlas.hpp"
#include "SunStudy/SunStudyPatchAtlas.hpp"

namespace evp::sunstudy {

// Triangle identities are local to a canonical element GUID, not a global face
// index. A changed triangle may retain its rectangle, never its sunlight values.
SunStudyAtlas BuildStableTriangleAtlas (const SampleGrid& grid, const geomsrv::Snapshot& snapshot,
                                        SunStudyPatchAtlas& allocations, const SunStudyPatchAtlas* previous = nullptr,
                                        const AtlasOptions& options = AtlasOptions ());

} // namespace evp::sunstudy
#endif
