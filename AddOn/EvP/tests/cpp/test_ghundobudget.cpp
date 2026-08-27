// Grasshopper/GhUndoBudget.cpp — what one Player run costs in undo presses.
//
// The loop under test is Tapioca panel -> Grasshopper -> Tapir -> Archicad, and
// the property is that it must not cost more undo presses than the work needed:
// one per element type or modification, not one per element.
//
// ⚠️ THE COUNT IS DERIVED, NOT READ. Archicad exposes no undo-stack count to an
// add-on — ACAPI_CallUndoableCommand is the only undo primitive in the AC29
// DevKit. What makes derivation sound is one fact read out of Tapir's own
// sources: every Tapir write command opens exactly one undoable scope, so one
// write REQUEST is one undo step. The live half of this test is a counting proxy
// in the worker that sees every request Tapir makes; this file is the RULE that
// judges the ledger it produces, and a rule that is arithmetic over a list of
// names should never need Archicad to test.

#include "Grasshopper/GhUndoBudget.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using evp::grasshopper::ClassifyCommand;
using evp::grasshopper::CommandClass;
using evp::grasshopper::DescribeUndoBudget;
using evp::grasshopper::EvaluateUndoBudget;
using evp::grasshopper::IsTapirReadCommand;
using evp::grasshopper::IsTapirWriteCommand;
using evp::grasshopper::UndoBudgetVerdict;
using evp::grasshopper::UndoLedgerEntry;

namespace {

UndoLedgerEntry Entry (const char* command, uint32_t invocations)
{
    UndoLedgerEntry entry;
    entry.command = command;
    entry.invocations = invocations;
    return entry;
}

} // namespace

TEST (GhUndoBudget, ProvenWritesAreWritesAndGetCommandsAreReads)
{
    EXPECT_EQ (CommandClass::Write, ClassifyCommand ("TapirCommand.MoveElements"));
    EXPECT_EQ (CommandClass::Write, ClassifyCommand ("TapirCommand.DeleteElements"));
    EXPECT_EQ (CommandClass::Write, ClassifyCommand ("TapirCommand.SetPropertyValuesOfElements"));

    EXPECT_EQ (CommandClass::Read, ClassifyCommand ("TapirCommand.GetSelectedElements"));
    EXPECT_EQ (CommandClass::Read, ClassifyCommand ("TapirCommand.GetAllElements"));
    EXPECT_TRUE (IsTapirReadCommand ("TapirCommand.GetLayers"));

    // Boundary cases of the sorted table, which is binary-searched.
    EXPECT_FALSE (IsTapirWriteCommand ("TapirCommand.AAAAAAA"));
    EXPECT_FALSE (IsTapirWriteCommand ("TapirCommand.ZZZZZZZ"));
}

TEST (GhUndoBudget, ACommandThatWritesThroughASharedHelperIsNotCalledARead)
{
    // ⚠️ THE CASE THAT CHANGED THIS RULE FROM TWO OUTCOMES TO THREE, pinned so it
    // cannot regress. Tapir's ElementCreationCommands open their undo scope in a
    // shared helper, so CreateDoors does not call ACAPI_CallUndoableCommand in
    // its own Execute and cannot be derived into the write table. It writes
    // anyway. A two-way table would have called it a read and reported a
    // door-per-element definition as costing nothing.
    EXPECT_FALSE (IsTapirWriteCommand ("TapirCommand.CreateDoors"));
    EXPECT_FALSE (IsTapirReadCommand ("TapirCommand.CreateDoors"));
    EXPECT_EQ (CommandClass::UnknownAddOn, ClassifyCommand ("TapirCommand.CreateDoors"));

    const UndoBudgetVerdict verdict = EvaluateUndoBudget ({ Entry ("TapirCommand.CreateDoors", 14) });
    EXPECT_EQ (14u, verdict.unknownWrites);
    EXPECT_EQ (0u, verdict.readInvocations);
    EXPECT_FALSE (verdict.withinBudget);
}

TEST (GhUndoBudget, TheTableIsSearchedCorrectlyAtBothEnds)
{
    // The lookup is a binary search over a sorted table; the first and last
    // entries are where an off-by-one in it shows up.
    EXPECT_TRUE (IsTapirWriteCommand ("TapirCommand.AddCommentToIssue"));
    EXPECT_TRUE (IsTapirWriteCommand ("TapirCommand.UpdateZones"));
}

TEST (GhUndoBudget, AnUnrecognisedTapirCommandIsAPossibleWriteRatherThanARead)
{
    // ⚠️ THE CONSERVATIVE CASE, AND THE ONE THAT MATTERS AFTER A TAPIR UPGRADE.
    // A new write command read as a read would report a run as clean while it
    // quietly cost undo steps.
    EXPECT_EQ (CommandClass::UnknownAddOn, ClassifyCommand ("TapirCommand.SomethingNewIn160"));
    EXPECT_FALSE (IsTapirReadCommand ("TapirCommand.SomethingNewIn160"));

    const UndoBudgetVerdict verdict = EvaluateUndoBudget ({ Entry ("TapirCommand.SomethingNewIn160", 3) });
    EXPECT_EQ (3u, verdict.unknownWrites);
    EXPECT_FALSE (verdict.withinBudget);
}

TEST (GhUndoBudget, TapiocaAndOfficialCommandsAreNotCountedAgainstTheBudget)
{
    // Tapioca's writes go through the dispatcher, which gives a whole batch ONE
    // undo scope. Counting them one-step-each here would erase the very
    // difference this measurement exists to show.
    EXPECT_EQ (CommandClass::Read, ClassifyCommand ("Tapioca.SetElementIds"));
    EXPECT_EQ (CommandClass::Read, ClassifyCommand ("API.GetSelectedElements"));

    const UndoBudgetVerdict verdict = EvaluateUndoBudget ({
        Entry ("Tapioca.SetElementIds", 200),
        Entry ("API.GetSelectedElements", 12),
    });
    EXPECT_EQ (0u, verdict.actualSteps);
    EXPECT_EQ (212u, verdict.readInvocations);
    EXPECT_TRUE (verdict.withinBudget);
}

TEST (GhUndoBudget, ARunWithNoWritesCostsNothingAndSaysSo)
{
    const UndoBudgetVerdict verdict = EvaluateUndoBudget ({ Entry ("TapirCommand.GetAllElements", 5) });
    EXPECT_EQ (0u, verdict.actualSteps);
    EXPECT_TRUE (verdict.withinBudget);
    EXPECT_NE (std::string::npos, DescribeUndoBudget (verdict).find ("no undo steps"));
}

TEST (GhUndoBudget, OneCallPerModificationIsWithinBudget)
{
    // THE TARGET SHAPE: a definition that moves every element in one call and
    // deletes in another costs two presses, and two is correct — the rule is one
    // per modification, not one overall.
    const UndoBudgetVerdict verdict = EvaluateUndoBudget ({
        Entry ("TapirCommand.MoveElements", 1),
        Entry ("TapirCommand.DeleteElements", 1),
        Entry ("TapirCommand.GetAllElements", 30),
    });

    EXPECT_EQ (2u, verdict.actualSteps);
    EXPECT_EQ (2u, verdict.idealSteps);
    EXPECT_EQ (0u, verdict.wastedSteps);
    EXPECT_TRUE (verdict.withinBudget);
    EXPECT_TRUE (verdict.offenders.empty ());
}

TEST (GhUndoBudget, AComponentLoopedPerElementIsOverBudgetByTheLoopCount)
{
    // THE FAILURE THIS EXISTS TO CATCH: a definition that calls MoveElements once
    // per element rather than once with a list. Forty elements, forty presses,
    // one modification's worth of work.
    const UndoBudgetVerdict verdict = EvaluateUndoBudget ({ Entry ("TapirCommand.MoveElements", 40) });

    EXPECT_EQ (40u, verdict.actualSteps);
    EXPECT_EQ (1u, verdict.idealSteps);
    EXPECT_EQ (39u, verdict.wastedSteps);
    EXPECT_FALSE (verdict.withinBudget);
    ASSERT_EQ (1u, verdict.offenders.size ());
    EXPECT_EQ ("TapirCommand.MoveElements x40", verdict.offenders[0]);
}

TEST (GhUndoBudget, OffendersAreWorstFirstAndSingleCallsAreNotOffenders)
{
    const UndoBudgetVerdict verdict = EvaluateUndoBudget ({
        Entry ("TapirCommand.MoveElements", 7),
        Entry ("TapirCommand.DeleteElements", 20),
        Entry ("TapirCommand.LockElements", 1),
    });

    EXPECT_EQ (28u, verdict.actualSteps);
    EXPECT_EQ (3u, verdict.idealSteps);
    ASSERT_EQ (2u, verdict.offenders.size ());
    EXPECT_EQ ("TapirCommand.DeleteElements x20", verdict.offenders[0]);
    EXPECT_EQ ("TapirCommand.MoveElements x7", verdict.offenders[1]);
    // Called once, so it cost exactly its share and naming it would be noise.
    for (const std::string& offender : verdict.offenders)
        EXPECT_EQ (std::string::npos, offender.find ("LockElements"));
}

TEST (GhUndoBudget, ZeroInvocationEntriesAreIgnoredRatherThanCountedAsAModification)
{
    // A ledger may carry a command the proxy saw declared but never invoked;
    // counting it would inflate idealSteps and quietly excuse a wasted press.
    const UndoBudgetVerdict verdict = EvaluateUndoBudget ({
        Entry ("TapirCommand.MoveElements", 0),
        Entry ("TapirCommand.DeleteElements", 2),
    });

    EXPECT_EQ (2u, verdict.actualSteps);
    EXPECT_EQ (1u, verdict.idealSteps);
    EXPECT_EQ (1u, verdict.wastedSteps);
}

TEST (GhUndoBudget, AnEmptyLedgerIsWithinBudget)
{
    const UndoBudgetVerdict verdict = EvaluateUndoBudget ({});
    EXPECT_EQ (0u, verdict.actualSteps);
    EXPECT_TRUE (verdict.withinBudget);
}

TEST (GhUndoBudget, TheReportNamesTheWindowItMeasured)
{
    // ⚠️ "SINCE THE LAST RUN" IS LOAD-BEARING WORDING. Tapir's writes fire from
    // their own button rather than from a solve, so a report saying "this run"
    // would be claiming a window that catches almost none of them.
    const UndoBudgetVerdict verdict = EvaluateUndoBudget ({ Entry ("TapirCommand.MoveElements", 4) });
    EXPECT_NE (std::string::npos, DescribeUndoBudget (verdict).find ("Since the last run"));
}

TEST (GhUndoBudget, TheReportNamesTheCostTheCauseAndTheFix)
{
    // DescribeUndoBudget is the whole product surface of this measurement, so
    // what it puts in front of a user is worth pinning: what it cost, what it
    // should have cost, which command did it, and what to do instead.
    const UndoBudgetVerdict verdict = EvaluateUndoBudget ({ Entry ("TapirCommand.MoveElements", 40) });
    const std::string text = DescribeUndoBudget (verdict);

    EXPECT_NE (std::string::npos, text.find ("40 undo step"));
    EXPECT_NE (std::string::npos, text.find ("1 would do"));
    EXPECT_NE (std::string::npos, text.find ("MoveElements x40"));
    EXPECT_NE (std::string::npos, text.find ("one call"));
}

TEST (GhUndoBudget, AWithinBudgetRunSaysSoRatherThanShowingNothing)
{
    const UndoBudgetVerdict verdict = EvaluateUndoBudget ({ Entry ("TapirCommand.MoveElements", 1) });
    EXPECT_NE (std::string::npos, DescribeUndoBudget (verdict).find ("which is the target"));
}

TEST (GhUndoBudget, TheWholeLoopIsAccountedForInOneVerdict)
{
    // A realistic Player run: read the selection, read the elements, move them
    // in one call, tag them one by one. The tagging is the bug, and the verdict
    // has to isolate it from the two calls that behaved.
    const UndoBudgetVerdict verdict = EvaluateUndoBudget ({
        Entry ("API.GetSelectedElements", 1),
        Entry ("TapirCommand.GetAllElements", 1),
        Entry ("TapirCommand.MoveElements", 1),
        Entry ("TapirCommand.SetPropertyValuesOfElements", 14),
    });

    EXPECT_EQ (15u, verdict.actualSteps);
    EXPECT_EQ (2u, verdict.idealSteps);
    EXPECT_EQ (13u, verdict.wastedSteps);
    EXPECT_EQ (2u, verdict.readInvocations);
    EXPECT_FALSE (verdict.withinBudget);
    ASSERT_EQ (1u, verdict.offenders.size ());
    EXPECT_EQ ("TapirCommand.SetPropertyValuesOfElements x14", verdict.offenders[0]);
}
