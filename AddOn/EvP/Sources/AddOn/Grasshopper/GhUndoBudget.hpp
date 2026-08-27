#ifndef EVP_GRASSHOPPER_GHUNDOBUDGET_HPP
#define EVP_GRASSHOPPER_GHUNDOBUDGET_HPP

// How many undo presses one Grasshopper run cost, and how many it needed to.
//
// Deliberately DevKit-free, Win32-free and CLR-free — only <cstdint>, <string>
// and <vector> — for the same reason GhProtocol.hpp and HostState.hpp are: this
// is the half of the undo story that can be proved offline. The instrument that
// produces the ledger has to run inside a live Archicad with a live Grasshopper;
// the RULE it is judged against is arithmetic over a list of names, and a rule
// like that should never need Archicad to test.
//
// ⚠️ WHY COUNTING IS POSSIBLE AT ALL. Archicad exposes no undo-stack count to an
// add-on — `ACAPI_CallUndoableCommand` is the only undo primitive in the AC29
// DevKit, and there is no begin/end pair, no grouping and no way to ask how deep
// the stack is. So the count is not read from Archicad; it is DERIVED, from the
// one fact that makes derivation sound:
//
//     every Tapir write command opens exactly one ACAPI_CallUndoableCommand,
//     so one write REQUEST is one undo step.
//
// That was read out of Tapir's own sources (57 commands, listed in the .cpp),
// not assumed. Count the write requests crossing the proxy and you have counted
// the undo steps, without Archicad's help.
//
// ⚠️ THE WRITE TABLE IS A FLOOR, NOT A CEILING, AND THAT IS WHY THE RULE HAS
// THREE OUTCOMES RATHER THAN TWO. It was derived from Tapir 1.5.8 by matching
// each command class to its Execute body and keeping the ones that call
// ACAPI_CallUndoableCommand directly. That derivation is provably INCOMPLETE:
// ElementCreationCommands.cpp opens its scope inside a shared helper, so
// CreateDoors, CreateWindows, ModifyWalls and their siblings write without the
// call appearing in their own Execute. Every one of them would have been
// classified as a read by a two-way table, and a run full of them would have
// been reported as clean while costing an undo press per element.
//
// So a command is a Write only when it is PROVEN to be one; a read only when its
// name makes that unambiguous (Tapir's Get* family); and UNKNOWN otherwise —
// counted as a possible write, reported separately, and enough on its own to
// stop a run being declared within budget. Getting a budget pessimistic is a
// nuisance. Reporting a run as clean because a write was not recognised is a
// wrong answer, and this file prefers the nuisance.

#include <cstdint>
#include <string>
#include <vector>

namespace evp {
namespace grasshopper {

// One command the proxy saw during a run, and how many times.
//
// `command` is the EFFECTIVE name: "TapirCommand.MoveElements" for an add-on
// command, "API.GetSelectedElements" for an official one. The proxy resolves
// API.ExecuteAddOnCommand down to the command it actually carried, because
// counting every Tapir call as one "ExecuteAddOnCommand" would hide the entire
// question.
struct UndoLedgerEntry {
    std::string command;
    uint32_t invocations = 0;
};

enum class CommandClass {
    Read,         // no undo step
    Write,        // proven to open exactly one undo step per invocation
    UnknownAddOn, // might write; see the header. Never assumed harmless.
};

CommandClass ClassifyCommand (const std::string& command);

// True for a command PROVEN to open an undoable scope. False does not mean
// "read" — see ClassifyCommand, which has a third answer.
bool IsTapirWriteCommand (const std::string& command);

// True for a Tapir command whose name makes it unambiguously a read.
bool IsTapirReadCommand (const std::string& command);

struct UndoBudgetVerdict {
    // One per write invocation, because each opens its own scope.
    uint32_t actualSteps = 0;

    // ⚠️ THE BUDGET RULE, AND IT IS THE USER'S WORDS: one step per element type
    // or modification. So the ideal is the number of DISTINCT write commands a
    // run used — moving forty elements is one modification and should cost one
    // press, not forty. A run that calls MoveElements once and DeleteElements
    // once is two, and that is correct rather than wasteful.
    uint32_t idealSteps = 0;

    uint32_t wastedSteps = 0;

    // Counted, never silently folded into either column. See the header warning.
    uint32_t unknownWrites = 0;

    uint32_t readInvocations = 0;

    bool withinBudget = false;

    // "TapirCommand.MoveElements x40", worst first. What a user has to act on.
    std::vector<std::string> offenders;
};

UndoBudgetVerdict EvaluateUndoBudget (const std::vector<UndoLedgerEntry>& ledger);

// The report a run shows. Kept beside the rule and tested with it, because the
// wording is the entire product surface of this measurement.
std::string DescribeUndoBudget (const UndoBudgetVerdict& verdict);

} // namespace grasshopper
} // namespace evp

#endif
