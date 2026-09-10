#ifndef GEOMETRYSERVER_PALETTE_WORKFLOWLOG_HPP
#define GEOMETRYSERVER_PALETTE_WORKFLOWLOG_HPP

// The workflow band's TRANSCRIPT: what was asked, what was solved, what came
// back, and what the definition complained about, oldest line first.
//
// ⚠️ IT EXISTS BECAUSE A SOLVED DEFINITION WAS OTHERWISE SILENT. Before this,
// a solution arrived, was stored, and produced exactly one word in the status
// line -- "solution 3 is current" -- with its outputs sitting in a struct
// nothing rendered. An author whose definition publishes a number had no way to
// see the number. A transcript is the smallest thing that answers "what did it
// just do", and it is also the only place a printed line CAN go: Grasshopper
// Player's Context Print publishes text, and text has no other home in a panel
// made of typed rows.
//
// ⚠️ IT IS READ AT A GLANCE OR IT IS NOT READ, AND THE FIRST VERSION WAS NOT.
// One line per tree ITEM, each repeating its id and its path, turned a solve
// with three outputs into a wall in which the one error was the least visible
// thing on screen. So: one BLOCK per solution, headed by its own line, values
// grouped under the id they belong to and aligned in a column, the root path
// "{0}" omitted because almost every value has it and a token every line
// carries distinguishes nothing, and messages in their own section under a
// heading that names their severity. The information is the same; finding it
// costs a glance instead of a search.
//
// ⚠️ DEVKIT-FREE, LIKE WorkflowRows AND InputModel, AND FOR THE SAME REASON.
// Every decision about what a line SAYS is made here and covered by
// tests/cpp/test_workflowlog.cpp; the band only owns the DG::MultiLineEdit the
// text lands in. A formatting rule that lived in the shell would be a rule no
// test can reach. That is also why the clock is a PARAMETER: this file never
// reads the time, so a test can state one.
//
// ⚠️ IT SHOWS THE LAST SOLUTION, NOT THE SESSION'S HISTORY. Recording a
// solution CLEARS what was there. Appending was tried and it made a log of ten
// blocks, nine of them answers to inputs that had since changed, with the
// current one at the bottom past the visible end of the box. The question a
// panel is asked is "what does it say now"; every earlier block is a different
// answer to it. Notes -- a load, a refusal, a cancellation -- accumulate until
// the next solution and then go with it, because "Loading ..." belongs to the
// load and not to the solve that followed.

#include "Grasshopper/GhWorkflowController.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace evp {

// How many lines are kept. A long session solving on every settle would grow
// without a bound otherwise, and the oldest lines are the least interesting
// ones -- the band shows the tail.
constexpr size_t MaxWorkflowLogLines = 400;

// How many values one solution contributes before the rest are summarised in a
// single line. A definition publishing a ten-thousand-point tree would otherwise
// flush every other line out of the buffer.
constexpr size_t MaxWorkflowLogOutputs = 60;

// How many items are listed under ONE id before that group is summarised. A
// group is a tree, and a tree is the case §9 wants inspectable -- but inspecting
// it does not require every leaf.
constexpr size_t MaxWorkflowLogItemsPerOutput = 12;

// The column the values line up in. Wide enough for an ordinary id, narrow
// enough to leave room for the value on a docked palette.
constexpr size_t WorkflowLogIdColumn = 16;

// The lines one accepted solution contributes: a header naming both revisions
// and the elapsed time, the grouped values, then the messages.
//
// `stamp` is the wall-clock time to show, or empty for none. It is passed in
// rather than read here so a test can state the time -- the same reason
// GhWorkflowController::Tick takes `nowMs`.
std::vector<std::string> FormatSolutionLines (const grasshopper::StoredSolution& solution, const std::string& stamp);

// The values of one solution, grouped by id in first-appearance order.
//
// A single-valued output is one line, `id  value`. An output holding several
// items becomes a header naming the count and one indented line per item,
// carrying the tree path that distinguishes them. The ROOT path is omitted
// throughout: `{0}` is what almost every value has, so printing it separates
// nothing and costs a column.
std::vector<std::string> FormatOutputBlock (const std::vector<grasshopper::protocol::SessionOutputValue>& outputs);

// The messages of one solution, under a heading per severity, errors first.
std::vector<std::string> FormatDiagnosticBlock (const std::vector<grasshopper::protocol::SessionDiagnostic>& list);

// The rolling buffer the band renders.
class WorkflowLog {
  public:
    // One free line -- a load, a failure, a cancellation.
    void Note (const std::string& line);

    // A solution's block, ONCE. A solution whose revision has already been
    // logged is ignored, so the band may poll on every idle tick without
    // repeating itself. Returns whether anything was added.
    bool Record (const grasshopper::StoredSolution& solution, const std::string& stamp);

    // Empties the box and LEAVES the already-logged mark alone.
    //
    // ⚠️ THE MARK IS WHY CLEAR APPEARED NOT TO WORK. Clear used to reset it as
    // well, so the very next idle tick found the current solution unlogged and
    // wrote it straight back -- the box emptied and refilled between two frames,
    // which reads as a button that only removes the notes. The mark is a
    // statement about what has been SEEN, and emptying the box does not unsee
    // anything.
    void Clear ();

    // Empties the box AND forgets what has been logged, so the next solution is
    // written again whatever its revision. For a new definition or a new
    // session: the previous one's block is about a document that is gone, and a
    // reopened session starts its own counter at one.
    void Forget ();

    // The whole transcript as one blob, newest line last.
    std::string Text () const;

    // Bumped by every change. The band compares it rather than the text: a
    // string comparison of a 400-line transcript on every idle tick is the kind
    // of cost that only shows up on someone else's machine.
    size_t Revision () const
    {
        return revision;
    }

    const std::vector<std::string>& Lines () const
    {
        return lines;
    }

  private:
    void Push (const std::string& line);

    std::vector<std::string> lines;
    size_t revision = 0;
    // The last solution revision written. Zero means none: the worker's own
    // counter starts at one (GhWorkflowController), so zero cannot collide with
    // a real solution.
    uint32_t loggedSolution = 0;
};

} // namespace evp

#endif
