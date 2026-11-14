#ifndef EVP_NATIVECOMMANDS_CAMERASETSTORE_HPP
#define EVP_NATIVECOMMANDS_CAMERASETSTORE_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "ProjectEnv/ArchicadCamera.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace geomsrv {

// Palette-session camera roles. ACAPI reads and writes stay in CameraSetCommands;
// this store only preserves declaration and capture order.
class CameraSetStore {
  public:
    struct State {
        GS::UniString owner;
        std::vector<std::pair<GS::UniString, std::vector<ArchicadCamera>>> values;
    };
    static CameraSetStore& Get ();

    void Configure (const GS::Array<GS::UniString>& names);
    void Configure (const GS::UniString& owner, const GS::Array<GS::UniString>& names);
    State Capture () const;
    void Restore (const State& state);
    GS::UniString Owner () const;
    uint64_t Revision () const
    {
        return revision;
    }
    void ResetValues ();
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
    GS::UniString owner;
    uint64_t revision = 1; // declarations, content edits and project invalidation
};

} // namespace geomsrv

#endif
