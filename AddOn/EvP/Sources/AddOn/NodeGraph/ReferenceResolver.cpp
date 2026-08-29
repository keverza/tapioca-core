#include "NodeGraph/ReferenceResolver.hpp"

namespace evp::nodegraph {

const char* ReferenceKindName (ReferenceKind kind)
{
    switch (kind) {
        case ReferenceKind::Element:
            return "element";
        case ReferenceKind::Property:
            return "property";
        case ReferenceKind::Attribute:
            return "attribute";
    }
    return "element";
}

const char* ResolutionStatusName (ResolutionStatus status)
{
    switch (status) {
        case ResolutionStatus::Resolved:
            return "resolved";
        case ResolutionStatus::Missing:
            return "missing";
        case ResolutionStatus::Ambiguous:
            return "ambiguous";
        case ResolutionStatus::Stale:
            return "stale";
        case ResolutionStatus::Incompatible:
            return "incompatible";
    }
    return "missing";
}

std::vector<ReferenceResolution> IReferenceResolver::ResolveAll (const std::vector<Reference>& references) const
{
    std::vector<ReferenceResolution> resolutions;
    resolutions.reserve (references.size ());
    for (const Reference& reference : references)
        resolutions.push_back (Resolve (reference));
    return resolutions;
}

ReferenceResolution UnavailableReferenceResolver::Resolve (const Reference& reference) const
{
    ReferenceResolution resolution;
    resolution.status = ResolutionStatus::Missing;
    resolution.detail = std::string (ReferenceKindName (reference.kind)) + " '" +
                        (reference.name.empty () ? reference.id : reference.name) + "' cannot be resolved: " + reason_;
    return resolution;
}

} // namespace evp::nodegraph
