#ifndef EVP_NATIVECOMMANDS_CAMERASETSTORE_HPP
#define EVP_NATIVECOMMANDS_CAMERASETSTORE_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "ProjectEnv/ArchicadCamera.hpp"

#include <vector>

namespace geomsrv {

// Palette-session camera roles. ACAPI reads and writes stay in CameraSetCommands;
// this store only preserves declaration and capture order.
class CameraSetStore {
  public:
    static CameraSetStore& Get ();

    void Configure (const GS::Array<GS::UniString>& names);
    void Clear ();
    bool IsDeclared (const GS::UniString& name) const;
    std::vector<ArchicadCamera> Values (const GS::UniString& name) const;
    bool Add (const GS::UniString& name, const ArchicadCamera& camera, GS::UniString& error);
    bool Update (const GS::UniString& name, size_t index, const ArchicadCamera& camera, GS::UniString& error);
    bool Remove (const GS::UniString& name, size_t index, GS::UniString& error);
    bool Clear (const GS::UniString& name, GS::UniString& error);

  private:
    struct Entry {
        GS::UniString name;
        std::vector<ArchicadCamera> cameras;
    };

    Entry* Find (const GS::UniString& name);
    const Entry* Find (const GS::UniString& name) const;
    std::vector<Entry> entries;
};

} // namespace geomsrv

#endif
