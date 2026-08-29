#ifndef EVP_NODEGRAPH_ARCHICADHOST_HPP
#define EVP_NODEGRAPH_ARCHICADHOST_HPP

// The one seam between the graph runtime and Archicad.
//
// Everything above this interface - the nodes, the plan, the evaluator, the
// reports - is DevKit-free and therefore covered by the offline suite. Exactly
// one translation unit implements it against ACAPI, and that one honours
// MainThreadGate's contract. A node never sees ACAPI, never sees a thread, and
// cannot call the SDK by accident.
//
// The interface is deliberately BATCHED. MainThreadGate measured round trips at
// roughly 0.6-8ms; a per-element interface turns a thousand-element selection
// into minutes of marshalling. Every call here takes or returns a whole list.

#include "NodeGraph/ProjectGenerations.hpp"
#include "NodeGraph/ReferenceResolver.hpp"
#include "NodeGraph/Value.hpp"

#include <string>
#include <vector>

namespace evp::nodegraph {

class IArchicadHost {
  public:
    virtual ~IArchicadHost () = default;

    // False when no project is open. Everything below then reports the same.
    virtual bool IsAvailable () const = 0;

    virtual const IProjectGenerationSource& Generations () const = 0;
    virtual const IReferenceResolver& References () const = 0;

    // The current selection, in Archicad's own order. An empty selection is
    // success with an empty list, not a failure - a graph that asks "what is
    // selected" when nothing is has a correct answer.
    virtual bool GetSelection (std::vector<ArchicadElementRef>& elements, std::string& error) const = 0;

    // Replaces the selection with `elements`. All-or-nothing at the API level is
    // not available, so the contract is: every reference is resolved before the
    // first change is made, and a reference that does not resolve fails the call
    // WITHOUT touching the selection.
    virtual bool SetSelection (const std::vector<ArchicadElementRef>& elements, std::string& error) = 0;
};

// The active host, or nullptr when the runtime is running without Archicad -
// the offline suite, a headless test, or the add-on before a project opens.
// Callers must treat nullptr as ordinary, not exceptional.
IArchicadHost* ActiveArchicadHost ();

// Installed once during add-on startup and cleared on teardown. Passing nullptr
// detaches, which is what makes "the project closed mid-run" expressible.
void SetActiveArchicadHost (IArchicadHost* host);

} // namespace evp::nodegraph

#endif
