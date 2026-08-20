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

    void Configure (const GS::Array<GS::UniString>& names);
    void Clear ();
    bool IsDeclared (const GS::UniString& name) const;
    GS::Array<GS::UniString> Names () const;
    GS::Array<GS::UniString> Values (const GS::UniString& name) const;
    bool Mutate (const GS::UniString& name, const GS::Array<GS::UniString>& guids,
                 Mutation mutation, GS::Int32& changed, GS::UniString& error);

private:
    struct Entry {
        GS::UniString name;
        GS::Array<GS::UniString> guids;
    };

    Entry* Find (const GS::UniString& name);
    const Entry* Find (const GS::UniString& name) const;

    GS::Array<Entry> entries;
};

} // namespace geomsrv

#endif
