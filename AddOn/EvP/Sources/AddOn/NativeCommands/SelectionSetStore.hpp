#ifndef EVP_NATIVECOMMANDS_SELECTIONSETSTORE_HPP
#define EVP_NATIVECOMMANDS_SELECTIONSETSTORE_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"

namespace geomsrv {

// Palette-session selection roles. This owns no DG items and performs no ACAPI call;
// the palette and SelectionCommands share it so embedded and external commands see
// the exact sets the user captured.
class SelectionSetStore {
  public:
    enum class Mutation { Replace, Add, Remove };

    static SelectionSetStore& Get ();

    // `exclusive`: an element may be in only ONE of these sets -- adding it to
    // one (update or add) removes it from every other.
    void Configure (const GS::Array<GS::UniString>& names, bool exclusive = false);
    void Clear ();
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
    bool exclusive = false;

    // Take every element of `kept` out of the other entries; returns how many.
    GS::Int32 RemoveFromOthers (const Entry& kept);
};

} // namespace geomsrv

#endif
