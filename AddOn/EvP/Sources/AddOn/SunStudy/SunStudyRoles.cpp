#include "SunStudy/SunStudyRoles.hpp"

#include <cctype>
#include <set>

namespace evp::sunstudy {

namespace {

std::set<std::string> CanonicalSet (const std::vector<std::string>& guids)
{
    std::set<std::string> out;
    for (const std::string& guid : guids) {
        const std::string canonical = CanonicalGuid (guid);
        if (!canonical.empty ())
            out.insert (canonical);
    }
    return out;
}

size_t CountUnmatched (const std::set<std::string>& picked, const std::set<std::string>& present)
{
    size_t unmatched = 0;
    for (const std::string& guid : picked)
        if (present.count (guid) == 0)
            ++unmatched;
    return unmatched;
}

} // namespace

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

ElementRoles ResolveElementRoles (const std::vector<std::string>& elements,
                                  const std::vector<std::string>& analysisPicked,
                                  const std::vector<std::string>& contextPicked)
{
    const std::set<std::string> analysis = CanonicalSet (analysisPicked);
    const std::set<std::string> context = CanonicalSet (contextPicked);

    ElementRoles out;
    out.roles.reserve (elements.size ());
    std::set<std::string> present;
    for (const std::string& element : elements) {
        const std::string guid = CanonicalGuid (element);
        present.insert (guid);

        ElementRole role;
        if (analysis.empty ())
            role = context.count (guid) > 0 ? ElementRole::Context : ElementRole::Analysis;
        else if (analysis.count (guid) > 0)
            role = ElementRole::Analysis;
        else if (context.empty () || context.count (guid) > 0)
            role = ElementRole::Context;
        else
            role = ElementRole::Ignored;

        out.roles.push_back (role);
        if (role == ElementRole::Analysis)
            ++out.analysis;
        else if (role == ElementRole::Context)
            ++out.context;
        else
            ++out.ignored;
    }

    out.unmatchedAnalysis = CountUnmatched (analysis, present);
    out.unmatchedContext = CountUnmatched (context, present);
    out.analysisNamedButAbsent = !analysis.empty () && out.analysis == 0;
    return out;
}

ElementRoles ResolveElementRoles (const geomsrv::Snapshot& snapshot, const std::vector<std::string>& analysisPicked,
                                  const std::vector<std::string>& contextPicked)
{
    std::vector<std::string> guids;
    guids.reserve (snapshot.meshes.size ());
    for (const geomsrv::Mesh& mesh : snapshot.meshes)
        guids.push_back (mesh.guid);
    return ResolveElementRoles (guids, analysisPicked, contextPicked);
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
