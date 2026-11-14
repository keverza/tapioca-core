#ifndef EVP_NATIVECOMMANDS_SELECTIONSETSTORE_HPP
#define EVP_NATIVECOMMANDS_SELECTIONSETSTORE_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include <cstdint>
#include <utility>
#include <vector>

namespace geomsrv {

// Palette-session selection roles. This owns no DG items and performs no ACAPI call;
// the palette and SelectionCommands share it so embedded and external commands see
// the exact sets the user captured.
class SelectionSetStore {
  public:
    enum class Mutation { Replace, Add, Remove };

    static SelectionSetStore& Get ();

    struct State {
        GS::UniString owner;
        std::vector<std::pair<GS::UniString, GS::Array<GS::UniString>>> values;
        bool exclusive = false;
    };
    // `exclusive`: an element may be in only ONE of these sets -- adding it to
    // one (update or add) removes it from every other.
    void Configure (const GS::Array<GS::UniString>& names, bool exclusive = false);
    // A same-owner/same-declaration launch retains captured inputs. Other owners
    // replace them; Capture/Restore isolate a temporary bus-launched command.
    void Configure (const GS::UniString& owner, const GS::Array<GS::UniString>& names, bool exclusive = false);
    State Capture () const;
    void Restore (const State& state);
    GS::UniString Owner () const;
    // Project boundaries drop old-document GUIDs but keep the selected command's
    // declarations/owner, and advance revisions to veto pending restoration.
    void ResetValues ();
    void Clear ();
    uint64_t Generation () const
    {
        return generation;
    }
    uint64_t Revision () const
    {
        return revision;
    }
    bool IsDeclared (const GS::UniString& name) const;
    GS::Array<GS::UniString> Names () const;
    GS::Array<GS::UniString> Values (const GS::UniString& name) const;
    // `moved` (optional) counts elements an EXCLUSIVE store took out of the
    // other sets to honour this mutation.
    bool Mutate (const GS::UniString& name, const GS::Array<GS::UniString>& guids, Mutation mutation,
                 GS::Int32& changed, GS::UniString& error, GS::Int32* moved = nullptr);

  private:
    struct Entry {
        GS::UniString name;
        GS::Array<GS::UniString> guids;
    };

    Entry* Find (const GS::UniString& name);
    const Entry* Find (const GS::UniString& name) const;

    GS::Array<Entry> entries;
    GS::UniString owner;
    bool exclusive = false;
    uint64_t generation = 1; // declaration lifetime, not content changes; main-thread-only
    uint64_t revision = 1;   // successful mutations/declarations; avoids copying sets on every follower tick

    // Take every element of `kept` out of the other entries; returns how many.
    GS::Int32 RemoveFromOthers (const Entry& kept);
};

} // namespace geomsrv

#endif
