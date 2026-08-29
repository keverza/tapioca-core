#ifndef EVP_NODEGRAPH_FAULTBARRIER_HPP
#define EVP_NODEGRAPH_FAULTBARRIER_HPP

// The barrier between node code and Archicad's process.
//
// A node body is the least trustworthy code in the runtime: it is where a bad
// index, a null deref or an unchecked cast lands. `try/catch(...)` is NOT enough
// on Windows. Under /EHsc an access violation is a structured exception, not a
// C++ one, and catch(...) is explicitly not required to catch it - so today's
// single try/catch in the evaluator turns a thrown std::exception into a failed
// node and turns a null deref into a dead Archicad with the user's project in it.
//
// RunGuarded closes that gap: C++ exceptions AND structured exceptions both
// become a returned failure with a message.
//
// The honest caveat: surviving an access violation means continuing in a process
// whose state was, for one instruction, wrong. That is a real risk and it is
// accepted deliberately, because the alternative here is not "a clean process",
// it is "Archicad terminates and the user loses unsaved work". The barrier
// records the fault so a repeatedly faulting node can be reported rather than
// silently retried.

#include <functional>
#include <string>

namespace evp::nodegraph {

struct GuardOutcome {
    // The callable ran to completion and returned normally.
    bool completed = false;

    // What the callable returned, meaningful only when completed is true.
    bool result = false;

    // Populated when completed is false.
    std::string fault;
};

// Runs `body` behind the barrier. Never throws, never lets a structured
// exception escape, and never returns without setting exactly one of
// completed/fault.
GuardOutcome RunGuarded (const std::function<bool ()>& body);

} // namespace evp::nodegraph

#endif
