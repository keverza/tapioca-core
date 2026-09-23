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
                                  const std::vector<std::string>& contextPicked,
                                  const std::vector<std::string>& ignoredPicked)
{
    const std::set<std::string> ignoredSet = CanonicalSet (ignoredPicked);
    // ⚠️ WHETHER A LIST WAS NAMED, DECIDED BEFORE THE IGNORE LIST THINS IT.
    // An analysis list whose every member is ignored is still a NAMED list;
    // branching on the thinned set would read it as "none named" and widen the
    // study to the whole model.
    const bool analysisNamed = !CanonicalSet (analysisPicked).empty ();
    const bool contextNamed = !CanonicalSet (contextPicked).empty ();
    // The table runs over what the explicit ignore list leaves: an element
    // ignored by name must not also count as a member of analysis or context,
    // or "analysis named but absent" would miss a list that is all ignored.
    std::set<std::string> analysis;
    for (const std::string& guid : CanonicalSet (analysisPicked))
        if (ignoredSet.count (guid) == 0)
            analysis.insert (guid);
    std::set<std::string> context;
    for (const std::string& guid : CanonicalSet (contextPicked))
        if (ignoredSet.count (guid) == 0)
            context.insert (guid);

    ElementRoles out;
    out.roles.reserve (elements.size ());
    std::set<std::string> present;
    for (const std::string& element : elements) {
        const std::string guid = CanonicalGuid (element);
        present.insert (guid);

        ElementRole role;
        if (ignoredSet.count (guid) > 0)
            role = ElementRole::Ignored;
        else if (!analysisNamed)
            role = context.count (guid) > 0 ? ElementRole::Context : ElementRole::Analysis;
        else if (analysis.count (guid) > 0)
            role = ElementRole::Analysis;
        else if (!contextNamed || context.count (guid) > 0)
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

    out.unmatchedAnalysis = CountUnmatched (CanonicalSet (analysisPicked), present);
    out.unmatchedContext = CountUnmatched (CanonicalSet (contextPicked), present);
    out.unmatchedIgnored = CountUnmatched (ignoredSet, present);
    // Named, and nothing of it left to measure -- absent from the model, or
    // every one of them ignored by name.
    out.analysisNamedButAbsent = analysisNamed && out.analysis == 0;
    return out;
}

ElementRoles ResolveElementRoles (const geomsrv::Snapshot& snapshot, const std::vector<std::string>& analysisPicked,
                                  const std::vector<std::string>& contextPicked,
                                  const std::vector<std::string>& ignoredPicked)
{
    std::vector<std::string> guids;
    guids.reserve (snapshot.meshes.size ());
    for (const geomsrv::Mesh& mesh : snapshot.meshes)
        guids.push_back (mesh.guid);
    return ResolveElementRoles (guids, analysisPicked, contextPicked, ignoredPicked);
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
