#include "SunStudy/SunStudyRoles.hpp"

#include <cctype>
#include <set>

namespace evp::sunstudy {

std::string CanonicalGuid (const std::string& guid)
{
    std::string out;
    out.reserve (guid.size ());
    for (const char c : guid) {
        if (c == '{' || c == '}' || std::isspace (static_cast<unsigned char> (c)))
            continue;
        out.push_back (static_cast<char> (std::toupper (static_cast<unsigned char> (c))));
    }
    return out;
}

std::vector<uint8_t> ElementRoles::SampleMask () const
{
    std::vector<uint8_t> mask (roles.size (), 0u);
    for (size_t i = 0; i < roles.size (); ++i)
        mask[i] = roles[i] == ElementRole::Analysis ? 1u : 0u;
    return mask;
}

std::shared_ptr<const geomsrv::Snapshot> OccluderSnapshot (const geomsrv::Snapshot& snapshot, const ElementRoles& roles)
{
    if (!roles.RestrictsOccluders () || roles.roles.size () != snapshot.meshes.size ())
        return nullptr;
    auto subset = std::make_shared<geomsrv::Snapshot> ();
    subset->id = snapshot.id;
    subset->scope = snapshot.scope;
    for (size_t m = 0; m < snapshot.meshes.size (); ++m)
        if (roles.roles[m] != ElementRole::Ignored)
            subset->meshes.push_back (snapshot.meshes[m]);
    return subset;
}

} // namespace evp::sunstudy
