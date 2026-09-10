// The workflow transcript's structure, its wording and its rolling buffer.
//
// Every rule these cover is one the panel cannot be asked about offline: the
// band owns a DG::MultiLineEdit and nothing else, exactly so that what a line
// SAYS is decided here.

#include "Palette/WorkflowLog.hpp"

#include <gtest/gtest.h>

using evp::FormatDiagnosticBlock;
using evp::FormatOutputBlock;
using evp::FormatSolutionLines;
using evp::WorkflowLog;
using evp::grasshopper::StoredSolution;
using evp::grasshopper::protocol::DiagnosticLevel;
using evp::grasshopper::protocol::SessionDiagnostic;
using evp::grasshopper::protocol::SessionOutputValue;

namespace {

SessionOutputValue Output (const std::string& id, const std::string& path, const std::string& value)
{
    SessionOutputValue output;
    output.id = id;
    output.type = "text";
    output.path = path;
    output.value = value;
    return output;
}

SessionDiagnostic Message (DiagnosticLevel level, const std::string& component, const std::string& text)
{
    SessionDiagnostic diagnostic;
    diagnostic.level = level;
    diagnostic.component = component;
    diagnostic.text = text;
    return diagnostic;
}

StoredSolution Solved (uint32_t solutionRevision, uint32_t requestRevision)
{
    StoredSolution solution;
    solution.solutionRevision = solutionRevision;
    solution.requestRevision = requestRevision;
    solution.elapsedMs = 42;
    return solution;
}

bool Contains (const std::vector<std::string>& lines, const std::string& fragment)
{
    for (const std::string& line : lines) {
        if (line.find (fragment) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

TEST (WorkflowLog, ASingleValuedOutputIsOneLine)
{
    const std::vector<std::string> lines = FormatOutputBlock ({ Output ("echo", "{0}", "test") });
    ASSERT_EQ (lines.size (), 1u);
    EXPECT_NE (lines[0].find ("echo"), std::string::npos);
    EXPECT_NE (lines[0].find ("test"), std::string::npos);
}

TEST (WorkflowLog, TheRootPathIsNotPrinted)
{
    // "{0}" is what almost every value has, so a column carrying it on every
    // line distinguishes nothing and costs the width the value needs.
    const std::vector<std::string> lines = FormatOutputBlock ({ Output ("echo", "{0}", "test") });
    ASSERT_EQ (lines.size (), 1u);
    EXPECT_EQ (lines[0].find ("{0}"), std::string::npos);
}

TEST (WorkflowLog, ANonRootPathIsPrintedBecauseItIsTheOnlyDistinguisher)
{
    const std::vector<std::string> lines = FormatOutputBlock ({ Output ("pts", "{0;3}", "1,2,3") });
    ASSERT_EQ (lines.size (), 1u);
    EXPECT_NE (lines[0].find ("{0;3}"), std::string::npos);
}

TEST (WorkflowLog, ValuesSharingAnIdAreGroupedUnderItOnce)
{
    // The fault this replaces: three items published under one id printed that
    // id three times, and a solve with three outputs became a wall.
    const std::vector<std::string> lines =
        FormatOutputBlock ({ Output ("txt", "{0}", "la"), Output ("txt", "{1}", "lab"), Output ("txt", "{2}", "labwl"),
                             Output ("echo", "{0}", "test") });

    // txt: one header plus three items. echo: one line.
    ASSERT_EQ (lines.size (), 5u);
    EXPECT_NE (lines[0].find ("txt"), std::string::npos);
    EXPECT_NE (lines[0].find ("3 items"), std::string::npos);
    EXPECT_EQ (lines[1].find ("txt"), std::string::npos) << "the id is not repeated per item";
    EXPECT_NE (lines[1].find ("la"), std::string::npos);
    EXPECT_NE (lines[4].find ("echo"), std::string::npos);
}

TEST (WorkflowLog, GroupsKeepTheOrderTheDefinitionPublishedThemIn)
{
    // Not alphabetical: the order is the author's own, and a report they laid
    // out deliberately must not be reordered by the panel.
    const std::vector<std::string> lines = FormatOutputBlock ({ Output ("zeta", "", "1"), Output ("alpha", "", "2") });
    ASSERT_EQ (lines.size (), 2u);
    EXPECT_NE (lines[0].find ("zeta"), std::string::npos);
    EXPECT_NE (lines[1].find ("alpha"), std::string::npos);
}

TEST (WorkflowLog, AnEmptyValueSaysSoRatherThanLeavingTheLineBlank)
{
    // "produced an empty string" is a real answer and a different one from
    // "produced nothing"; a blank cell reads as a formatting fault.
    const std::vector<std::string> lines = FormatOutputBlock ({ Output ("Content", "{0}", "") });
    ASSERT_EQ (lines.size (), 1u);
    EXPECT_NE (lines[0].find ("(empty)"), std::string::npos);
}

TEST (WorkflowLog, ALongGroupIsSummarisedRatherThanListedWhole)
{
    std::vector<SessionOutputValue> outputs;
    for (size_t index = 0; index < evp::MaxWorkflowLogItemsPerOutput + 5; ++index)
        outputs.push_back (Output ("pts", "{" + std::to_string (index) + "}", "1"));

    const std::vector<std::string> lines = FormatOutputBlock (outputs);
    // Header + the per-group cap + one summary line.
    EXPECT_EQ (lines.size (), evp::MaxWorkflowLogItemsPerOutput + 2);
    EXPECT_NE (lines.back ().find ("5 more"), std::string::npos);
}

TEST (WorkflowLog, ManyDistinctOutputsAreSummarisedRatherThanDropped)
{
    std::vector<SessionOutputValue> outputs;
    for (size_t index = 0; index < evp::MaxWorkflowLogOutputs + 7; ++index)
        outputs.push_back (Output ("out" + std::to_string (index), "", "1"));

    const std::vector<std::string> lines = FormatOutputBlock (outputs);
    EXPECT_TRUE (Contains (lines, "more outputs not listed"));
    EXPECT_LE (lines.size (), evp::MaxWorkflowLogOutputs + 1);
}

TEST (WorkflowLog, OutputNewlinesStayOnOneLine)
{
    // A printed value holding a line break would otherwise become two transcript
    // lines, the second with no id in front of it.
    const std::vector<std::string> lines = FormatOutputBlock ({ Output ("Report", "{0}", "first\nsecond") });
    ASSERT_EQ (lines.size (), 1u);
    EXPECT_EQ (lines[0].find ('\n'), std::string::npos);
    EXPECT_NE (lines[0].find ("first / second"), std::string::npos);
}

TEST (WorkflowLog, LongValueIsClippedOnACharacterBoundary)
{
    // Three-byte characters, cut mid-sequence by a naive byte cut.
    std::string wide;
    for (int index = 0; index < 400; ++index)
        wide += "\xE2\x9C\x93"; // U+2713

    const std::vector<std::string> lines = FormatOutputBlock ({ Output ("Marks", "", wide) });
    ASSERT_EQ (lines.size (), 1u);
    ASSERT_NE (lines[0].find ("..."), std::string::npos);

    // Everything after the padded id column, minus the ellipsis.
    const std::string tail = lines[0].substr (lines[0].find ("\xE2\x9C\x93"));
    const std::string body = tail.substr (0, tail.size () - 3);
    ASSERT_EQ (body.size () % 3, 0u) << "clipped inside a multi-byte character";
}

TEST (WorkflowLog, MessagesAreGroupedUnderASeverityHeadingErrorsFirst)
{
    const std::vector<std::string> lines = FormatDiagnosticBlock (
        { Message (DiagnosticLevel::Remark, "A", "noted"), Message (DiagnosticLevel::Error, "B", "broke"),
          Message (DiagnosticLevel::Warning, "C", "careful") });

    ASSERT_EQ (lines.size (), 6u);
    EXPECT_NE (lines[0].find ("errors"), std::string::npos);
    EXPECT_NE (lines[1].find ("broke"), std::string::npos);
    EXPECT_NE (lines[2].find ("warnings"), std::string::npos);
    EXPECT_NE (lines[4].find ("remarks"), std::string::npos);
}

TEST (WorkflowLog, TheComponentGuidIsNotShown)
{
    // 38 characters of GUID in front of every message is most of the reason the
    // first transcript was unreadable. The nickname is what a reader looks for.
    const std::vector<std::string> lines = FormatDiagnosticBlock (
        { Message (DiagnosticLevel::Error, "Filter {c03d286e-9936-4536-8767-517f4dfafef3}", "only one value") });

    ASSERT_EQ (lines.size (), 2u);
    EXPECT_NE (lines[1].find ("Filter"), std::string::npos);
    EXPECT_EQ (lines[1].find ("c03d286e"), std::string::npos);
    EXPECT_NE (lines[1].find ("only one value"), std::string::npos);
}

TEST (WorkflowLog, AHeaderNamesBothRevisionsTheTimeAndTheCost)
{
    // §7's two counters. A transcript that reported only the solution's own
    // number could not be lined up against the input change that caused it.
    const std::vector<std::string> lines = FormatSolutionLines (Solved (3, 7), "15:42:07");
    ASSERT_GE (lines.size (), 1u);
    EXPECT_NE (lines[0].find ("15:42:07"), std::string::npos);
    EXPECT_NE (lines[0].find ("solution 3"), std::string::npos);
    EXPECT_NE (lines[0].find ("request 7"), std::string::npos);
    EXPECT_NE (lines[0].find ("42 ms"), std::string::npos);
}

TEST (WorkflowLog, AHeaderWithoutAClockSimplyOmitsIt)
{
    const std::vector<std::string> lines = FormatSolutionLines (Solved (1, 1), std::string ());
    EXPECT_EQ (lines[0].substr (0, 8), "solution");
}

TEST (WorkflowLog, ASolutionWithNothingToSayStillSaysSo)
{
    // A definition with no output component solves perfectly and publishes
    // nothing, which looks exactly like a silent failure.
    const std::vector<std::string> lines = FormatSolutionLines (Solved (1, 1), "10:00:00");
    EXPECT_TRUE (Contains (lines, "publishes no outputs"));
}

TEST (WorkflowLog, ASolutionWithValuesGetsAnOutputsHeading)
{
    StoredSolution solution = Solved (2, 2);
    solution.outputs.push_back (Output ("echo", "{0}", "test"));
    solution.diagnostics.push_back (Message (DiagnosticLevel::Error, "Filter", "only one value"));

    const std::vector<std::string> lines = FormatSolutionLines (solution, "10:00:00");
    EXPECT_TRUE (Contains (lines, "  outputs"));
    EXPECT_TRUE (Contains (lines, "  errors"));
    EXPECT_FALSE (Contains (lines, "publishes no outputs"));
}

TEST (WorkflowLog, ASolutionIsRecordedOnceHoweverOftenItIsPolled)
{
    // The band polls on every idle tick; a transcript that repeated itself
    // sixty times a second would be useless within a second.
    WorkflowLog log;
    const StoredSolution solution = Solved (4, 4);

    EXPECT_TRUE (log.Record (solution, "10:00:00"));
    const size_t after = log.Lines ().size ();
    EXPECT_FALSE (log.Record (solution, "10:00:01"));
    EXPECT_FALSE (log.Record (solution, "10:00:02"));
    EXPECT_EQ (log.Lines ().size (), after);
}

TEST (WorkflowLog, RecordingASolutionREPLACESWhatWasThere)
{
    // Autoclear. Ten blocks of history, nine of them answers to inputs that have
    // since changed, is what this replaces -- and the current one was at the
    // bottom, past the visible end of the box.
    WorkflowLog log;
    log.Note ("Loading something.gh ...");
    ASSERT_TRUE (log.Record (Solved (1, 1), "10:00:00"));
    const size_t first = log.Lines ().size ();
    EXPECT_EQ (log.Text ().find ("Loading"), std::string::npos) << "the note was cleared with the rest";

    ASSERT_TRUE (log.Record (Solved (2, 2), "10:00:05"));
    // The same block, and only it: the note went with the solve it preceded,
    // and the previous block is not above the new one.
    EXPECT_EQ (log.Lines ().size (), first);
    EXPECT_EQ (log.Text ().find ("solution 1"), std::string::npos);
    EXPECT_NE (log.Text ().find ("solution 2"), std::string::npos);
}

TEST (WorkflowLog, AnOlderSolutionDoesNotReopenTheTranscript)
{
    // A stale result arriving after a newer one is dropped by the controller;
    // if one ever reaches here it must not scroll the newest solution away.
    WorkflowLog log;
    EXPECT_TRUE (log.Record (Solved (5, 5), "10:00:00"));
    EXPECT_FALSE (log.Record (Solved (4, 4), "10:00:01"));
}

TEST (WorkflowLog, ForgetLetsANewSessionCountFromOneAgain)
{
    WorkflowLog log;
    EXPECT_TRUE (log.Record (Solved (9, 9), "10:00:00"));

    log.Forget ();
    EXPECT_TRUE (log.Lines ().empty ());
    // A reopened session's counter starts again; without the reset its first
    // solution would look older than the previous session's last.
    EXPECT_TRUE (log.Record (Solved (1, 1), "10:00:01"));
}

TEST (WorkflowLog, ClearDoesNotLetTheSameSolutionComeStraightBack)
{
    // The measured fault: Clear reset the logged mark too, so the next idle
    // tick found the current solution unlogged and wrote it back. The box
    // emptied and refilled between two frames, which reads as a Clear button
    // that only removes the notes above the block.
    WorkflowLog log;
    ASSERT_TRUE (log.Record (Solved (3, 3), "10:00:00"));

    log.Clear ();
    EXPECT_TRUE (log.Lines ().empty ());
    EXPECT_FALSE (log.Record (Solved (3, 3), "10:00:01")) << "the same solution is still logged";
    EXPECT_TRUE (log.Lines ().empty ());

    // A NEWER one still arrives, because clearing the box says nothing about
    // what happens next.
    EXPECT_TRUE (log.Record (Solved (4, 4), "10:00:02"));
}

TEST (WorkflowLog, TheBufferKeepsTheTailAndNotTheHead)
{
    WorkflowLog log;
    for (size_t index = 0; index < evp::MaxWorkflowLogLines + 50; ++index)
        log.Note (std::to_string (index));

    EXPECT_EQ (log.Lines ().size (), evp::MaxWorkflowLogLines);
    EXPECT_EQ (log.Lines ().back (), std::to_string (evp::MaxWorkflowLogLines + 49));
    EXPECT_EQ (log.Lines ().front (), std::to_string (50));
}

TEST (WorkflowLog, RevisionMovesOnEveryChangeSoThePanelCanCompareIt)
{
    WorkflowLog log;
    const size_t start = log.Revision ();

    log.Note ("one");
    EXPECT_NE (log.Revision (), start);

    const size_t afterNote = log.Revision ();
    EXPECT_FALSE (log.Record (Solved (0, 0), "10:00:00")); // revision 0 is "no solution"
    EXPECT_EQ (log.Revision (), afterNote);
}

TEST (WorkflowLog, TextJoinsWithNewlinesAndDoesNotTrail)
{
    WorkflowLog log;
    log.Note ("one");
    log.Note ("two");
    EXPECT_EQ (log.Text (), "one\ntwo");
}
