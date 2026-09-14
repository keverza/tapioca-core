#include "NativeCommands/CameraSetStore.hpp"

#include <cctype>

namespace {

std::string Key (const GS::UniString& value)
{
    std::string key (value.ToCStr (0, GS::MaxUSize, CC_UTF8).Get ());
    for (char& c : key)
        c = static_cast<char> (std::toupper (static_cast<unsigned char> (c)));
    return key;
}

} // namespace

namespace geomsrv {

CameraSetStore& CameraSetStore::Get ()
{
    static CameraSetStore store;
    return store;
}

void CameraSetStore::Configure (const GS::Array<GS::UniString>& names)
{
    entries.clear ();
    for (const GS::UniString& name : names)
        entries.push_back ({ name, {} });
}

void CameraSetStore::Clear ()
{
    entries.clear ();
}

CameraSetStore::Entry* CameraSetStore::Find (const GS::UniString& name)
{
    const std::string wanted = Key (name);
    for (Entry& entry : entries) {
        if (Key (entry.name) == wanted)
            return &entry;
    }
    return nullptr;
}

const CameraSetStore::Entry* CameraSetStore::Find (const GS::UniString& name) const
{
    return const_cast<CameraSetStore*> (this)->Find (name);
}

bool CameraSetStore::IsDeclared (const GS::UniString& name) const
{
    return Find (name) != nullptr;
}

std::vector<ArchicadCamera> CameraSetStore::Values (const GS::UniString& name) const
{
    const Entry* entry = Find (name);
    return entry != nullptr ? entry->cameras : std::vector<ArchicadCamera> {};
}

bool CameraSetStore::Add (const GS::UniString& name, const ArchicadCamera& camera, GS::UniString& error)
{
    Entry* entry = Find (name);
    if (entry == nullptr) {
        error = "camera set is not declared for the active command";
        return false;
    }
    entry->cameras.push_back (camera);
    return true;
}

bool CameraSetStore::Update (const GS::UniString& name, size_t index, const ArchicadCamera& camera,
                             GS::UniString& error)
{
    Entry* entry = Find (name);
    if (entry == nullptr) {
        error = "camera set is not declared for the active command";
        return false;
    }
    if (index >= entry->cameras.size ()) {
        error = "the selected camera no longer exists";
        return false;
    }
    entry->cameras[index] = camera;
    return true;
}

bool CameraSetStore::Remove (const GS::UniString& name, size_t index, GS::UniString& error)
{
    Entry* entry = Find (name);
    if (entry == nullptr) {
        error = "camera set is not declared for the active command";
        return false;
    }
    if (index >= entry->cameras.size ()) {
        error = "the selected camera no longer exists";
        return false;
    }
    entry->cameras.erase (entry->cameras.begin () + static_cast<std::ptrdiff_t> (index));
    return true;
}

bool CameraSetStore::Clear (const GS::UniString& name, GS::UniString& error)
{
    Entry* entry = Find (name);
    if (entry == nullptr) {
        error = "camera set is not declared for the active command";
        return false;
    }
    entry->cameras.clear ();
    return true;
}

} // namespace geomsrv
