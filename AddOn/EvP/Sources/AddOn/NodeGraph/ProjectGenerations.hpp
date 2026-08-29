#ifndef EVP_NODEGRAPH_PROJECTGENERATIONS_HPP
#define EVP_NODEGRAPH_PROJECTGENERATIONS_HPP

// Archicad state is an implicit input.
//
// A node that reads the model has inputs the graph cannot see. Keying its cache
// on ports and parameters alone means it serves a stale answer forever after the
// user changes the model - the defect that makes a BIM graph untrustworthy.
//
// So a node DECLARES which generation domains it reads, the plan samples exactly
// those once per run, and the evaluator folds them into that node's cache key. A
// selection that did not change is a cache hit; one that did is a re-execution.
// No polling, no notification plumbing, and nothing to keep in sync.
//
// Only domains with a real source exist here. The architecture document lists
// seven; adding one without something that can answer it produces a node whose
// cache is silently wrong, which is worse than not having the node. Add a domain
// in the same change as its source and the node that needs it.

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace evp::nodegraph {

enum class GenerationDomain {
    // Bumped when the project is created, opened or closed. Everything derived
    // from the model is invalid across that boundary.
    Project,

    // A value derived from the current selection, so it changes exactly when the
    // selection does.
    Selection,
};

const char* GenerationDomainName (GenerationDomain domain);

// A node's declared dependencies. A tiny set rather than a bitmask so the
// catalog can name them to a client without a decoder ring.
class GenerationSet {
  public:
    GenerationSet () = default;
    GenerationSet (std::initializer_list<GenerationDomain> domains) : domains_ (domains)
    {
    }

    bool Contains (GenerationDomain domain) const;
    bool Empty () const
    {
        return domains_.empty ();
    }
    const std::vector<GenerationDomain>& Domains () const
    {
        return domains_;
    }

    void Add (GenerationDomain domain);

  private:
    std::vector<GenerationDomain> domains_;
};

// What the sampled domains were worth at the start of one run. Sampling happens
// once per run, not once per node: two nodes reading the selection in the same
// run must see the same selection, or the run is not internally consistent.
class GenerationSample {
  public:
    void Set (GenerationDomain domain, uint64_t value);

    // Zero when the domain was not sampled. A node that declared a domain the
    // plan could not sample is rejected before it runs, so zero never silently
    // means "unchanged".
    uint64_t Value (GenerationDomain domain) const;

    bool Has (GenerationDomain domain) const;

  private:
    std::vector<std::pair<GenerationDomain, uint64_t>> values_;
};

// Answers "what is this domain worth right now". The Archicad implementation
// marshals onto the host thread; the offline default answers nothing, which is
// what makes a graph that needs Archicad fail at the door rather than mid-run.
class IProjectGenerationSource {
  public:
    virtual ~IProjectGenerationSource () = default;

    // False when this source cannot answer for `domain` - a project is not open,
    // or the host is absent. `error` explains it to the user.
    virtual bool Sample (GenerationDomain domain, uint64_t& value, std::string& error) const = 0;
};

} // namespace evp::nodegraph

#endif
