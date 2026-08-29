#ifndef EVP_NODEGRAPH_REFERENCERESOLVER_HPP
#define EVP_NODEGRAPH_REFERENCERESOLVER_HPP

// A persistent graph names Archicad resources by stable identity, and those
// identities go stale.
//
// A saved graph outlives the project state it was authored against. An element
// is deleted, a project is replaced, a file is reopened from a colleague's copy.
// A bare GUID string - which is all ArchicadElementRef used to be - cannot tell
// "this element" from "an element that no longer exists", so the graph either
// silently skips it or fails deep inside a run with a GUID in the message.
//
// Resolution turns that into a stated status BEFORE anything executes. Names are
// carried as repair metadata only: they are how a person recognizes what was
// meant, never how the runtime finds it.

#include "NodeGraph/Value.hpp"

#include <string>
#include <vector>

namespace evp::nodegraph {

enum class ReferenceKind {
    Element,
    Property,
    Attribute,
};

const char* ReferenceKindName (ReferenceKind kind);

struct Reference {
    ReferenceKind kind = ReferenceKind::Element;

    // The stable identity. A GUID for an element; the host decides for the rest.
    std::string id;

    // Repair metadata. What this was called when the graph was authored, so a
    // person can recognize what a broken reference meant. Never used to resolve.
    std::string name;
};

enum class ResolutionStatus {
    // Found, and usable as-is.
    Resolved,

    // The identity is not in the current project.
    Missing,

    // The identity matched more than one resource - possible after a merge.
    Ambiguous,

    // Found, but the project has moved on in a way that makes the cached
    // understanding of it untrustworthy.
    Stale,

    // Found, but not of a kind this reference can be used as.
    Incompatible,
};

const char* ResolutionStatusName (ResolutionStatus status);

struct ReferenceResolution {
    ResolutionStatus status = ResolutionStatus::Missing;

    // Why, in words a person can act on. Always populated for anything that is
    // not Resolved.
    std::string detail;

    // What the resource is called now, which may differ from Reference::name.
    // That difference is the useful part of a repair dialogue.
    std::string currentName;

    bool Usable () const
    {
        return status == ResolutionStatus::Resolved;
    }
};

class IReferenceResolver {
  public:
    virtual ~IReferenceResolver () = default;

    virtual ReferenceResolution Resolve (const Reference& reference) const = 0;

    // Resolve many at once, returning one resolution per input in order.
    //
    // This exists because the Archicad implementation crosses onto the host
    // thread, and MainThreadGate measured that crossing at roughly 0.6-8ms. A
    // thousand-element selection resolved one at a time is seconds of pure
    // marshalling. The default loops, so a resolver with no crossing to amortize
    // does not have to implement it.
    virtual std::vector<ReferenceResolution> ResolveAll (const std::vector<Reference>& references) const;
};

// The resolver in force when no Archicad project is reachable. It answers
// Missing with a reason rather than pretending: a graph evaluated with no host
// must say so, not quietly behave as if every reference were fine.
class UnavailableReferenceResolver final : public IReferenceResolver {
  public:
    explicit UnavailableReferenceResolver (std::string reason = "no Archicad project is available")
        : reason_ (std::move (reason))
    {
    }

    ReferenceResolution Resolve (const Reference& reference) const override;

  private:
    std::string reason_;
};

} // namespace evp::nodegraph

#endif
