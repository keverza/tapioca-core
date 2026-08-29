#include "NodeGraph/ProjectGenerations.hpp"

#include <algorithm>

namespace evp::nodegraph {

const char* GenerationDomainName (GenerationDomain domain)
{
    switch (domain) {
        case GenerationDomain::Project:
            return "project";
        case GenerationDomain::Selection:
            return "selection";
    }
    return "project";
}

bool GenerationSet::Contains (GenerationDomain domain) const
{
    return std::find (domains_.begin (), domains_.end (), domain) != domains_.end ();
}

void GenerationSet::Add (GenerationDomain domain)
{
    if (!Contains (domain))
        domains_.push_back (domain);
}

void GenerationSample::Set (GenerationDomain domain, uint64_t value)
{
    for (auto& entry : values_) {
        if (entry.first == domain) {
            entry.second = value;
            return;
        }
    }
    values_.emplace_back (domain, value);
}

uint64_t GenerationSample::Value (GenerationDomain domain) const
{
    for (const auto& entry : values_) {
        if (entry.first == domain)
            return entry.second;
    }
    return 0;
}

bool GenerationSample::Has (GenerationDomain domain) const
{
    for (const auto& entry : values_) {
        if (entry.first == domain)
            return true;
    }
    return false;
}

} // namespace evp::nodegraph
