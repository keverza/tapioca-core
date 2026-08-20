#include "NativeCommands/SelectionSetStore.hpp"

#include <cctype>
#include <string>

namespace {

std::string Key (const GS::UniString& value)
{
    std::string out (value.ToCStr (0, GS::MaxUSize, CC_UTF8).Get ());
    if (out.size () >= 2 && out.front () == '{' && out.back () == '}')
        out = out.substr (1, out.size () - 2);
    for (char& c : out)
        c = (char) std::toupper ((unsigned char) c);
    return out;
}

bool Contains (const GS::Array<GS::UniString>& values, const GS::UniString& value)
{
    const std::string wanted = Key (value);
    for (const GS::UniString& existing : values) {
        if (Key (existing) == wanted)
            return true;
    }
    return false;
}

} // namespace

namespace geomsrv {

SelectionSetStore& SelectionSetStore::Get ()
{
    static SelectionSetStore store;
    return store;
}

void SelectionSetStore::Configure (const GS::Array<GS::UniString>& names)
{
    entries.Clear ();
    for (const GS::UniString& name : names) {
        Entry entry;
        entry.name = name;
        entries.Push (entry);
    }
}

void SelectionSetStore::Clear () { entries.Clear (); }

SelectionSetStore::Entry* SelectionSetStore::Find (const GS::UniString& name)
{
    const std::string wanted = Key (name);
    for (Entry& entry : entries) {
        if (Key (entry.name) == wanted)
            return &entry;
    }
    return nullptr;
}

const SelectionSetStore::Entry* SelectionSetStore::Find (const GS::UniString& name) const
{
    return const_cast<SelectionSetStore*> (this)->Find (name);
}

bool SelectionSetStore::IsDeclared (const GS::UniString& name) const { return Find (name) != nullptr; }

GS::Array<GS::UniString> SelectionSetStore::Names () const
{
    GS::Array<GS::UniString> names;
    for (const Entry& entry : entries)
        names.Push (entry.name);
    return names;
}

GS::Array<GS::UniString> SelectionSetStore::Values (const GS::UniString& name) const
{
    GS::Array<GS::UniString> values;
    const Entry* entry = Find (name);
    if (entry != nullptr)
        values = entry->guids;
    return values;
}

bool SelectionSetStore::Mutate (const GS::UniString& name, const GS::Array<GS::UniString>& guids,
                                Mutation mutation, GS::Int32& changed, GS::UniString& error)
{
    Entry* entry = Find (name);
    if (entry == nullptr) {
        error = "selection set is not declared for the active command: " + name;
        return false;
    }

    changed = 0;
    if (mutation == Mutation::Replace) {
        GS::Array<GS::UniString> unique;
        for (const GS::UniString& guid : guids) {
            if (!Contains (unique, guid))
                unique.Push (guid);
        }
        changed = (GS::Int32) unique.GetSize ();
        entry->guids = unique;
        return true;
    }
    if (mutation == Mutation::Add) {
        for (const GS::UniString& guid : guids) {
            if (!Contains (entry->guids, guid)) {
                entry->guids.Push (guid);
                ++changed;
            }
        }
        return true;
    }
    for (const GS::UniString& guid : guids) {
        const std::string wanted = Key (guid);
        for (UIndex i = 0; i < entry->guids.GetSize (); ++i) {
            if (Key (entry->guids[i]) == wanted) {
                entry->guids.Delete (i);
                ++changed;
                break;
            }
        }
    }
    return true;
}

} // namespace geomsrv
