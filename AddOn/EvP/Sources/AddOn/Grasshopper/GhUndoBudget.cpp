#include "GhUndoBudget.hpp"

#include <algorithm>

namespace evp {
namespace grasshopper {

namespace {

constexpr const char* TapirNamespace = "TapirCommand.";
constexpr const char* TapiocaNamespace = "Tapioca.";
constexpr const char* OfficialNamespace = "API.";

// ⚠️ DERIVED FROM TAPIR'S SOURCE, NOT COMPILED FROM MEMORY, AND KNOWN TO BE
// INCOMPLETE. Every entry is a command whose Execute calls
// ACAPI_CallUndoableCommand directly in
// tapir-archicad-automation-main/archicad-addon/Sources at the pinned 1.5.8.
// Commands that open their scope through a shared helper — the whole
// ElementCreationCommands family — are NOT here and land in UnknownAddOn, which
// is the safe side. See the header for why the rule has three outcomes.
//
// Regenerate the same way after a Tapir upgrade rather than editing by hand:
// match each `<Class>Command::GetName` to its `<Class>Command::Execute` body and
// keep the ones that open an undoable scope. Widening it is welcome; every name
// moved from Unknown to Write makes a budget sharper.
//
// Sorted, because ClassifyCommand binary-searches it and because a sorted list
// is the only kind a human can diff after a regeneration.
constexpr const char* TapirWriteCommands[] = {
    "AddCommentToIssue",
    "ApplyFavoritesToElementDefaults",
    "ApplyFavoritesToElements",
    "AttachElementsToIssue",
    "CreateClassificationItems",
    "CreateClassificationSystems",
    "CreateDesignOptionCombinations",
    "CreateDesignOptionSets",
    "CreateDesignOptions",
    "CreateDrawings",
    "CreateFavoritesFromElements",
    "CreateGroups",
    "CreateIssue",
    "CreateKeynoteFolders",
    "CreateKeynoteItems",
    "CreateKeynoteLabels",
    "CreateMEPElements",
    "CreateMEPRoutingElements",
    "CreateProjectInfoFields",
    "CreatePropertyDefinitions",
    "CreatePropertyGroups",
    "CreateSolidElementLinks",
    "DeleteClassificationItems",
    "DeleteClassificationSystems",
    "DeleteElements",
    "DeleteFavorites",
    "DeleteIssue",
    "DeleteKeynoteFolders",
    "DeleteKeynoteItems",
    "DeleteProjectInfoFields",
    "DeletePropertyDefinitions",
    "DeletePropertyGroups",
    "DetachElementsFromIssue",
    "ImportIssuesFromBCF",
    "LockElements",
    "ModifyKeynoteFolders",
    "ModifyKeynoteItems",
    "ModifyMeshes",
    "MoveDesignOptionsToAnotherSet",
    "MoveElements",
    "MoveElementsToDesignOptions",
    "MoveNavigatorItem",
    "RemoveSolidElementLinks",
    "RenameFavorites",
    "RotateElements",
    "Set3DCutPlanes",
    "SetActiveDesignOptionsInCombinations",
    "SetClassificationsOfElements",
    "SetDetailsOfElements",
    "SetGDLParametersOfElements",
    "SetGeoLocation",
    "SetPropertyValuesOfAttributes",
    "SetPropertyValuesOfElements",
    "UnlockElements",
    "UpdateFavoritesFromElements",
    "UpdatePropertyDefinitions",
    "UpdateZones",
};

bool StartsWith (const std::string& text, const char* prefix)
{
    const std::string value (prefix);
    return text.size () >= value.size () && text.compare (0, value.size (), value) == 0;
}

std::string After (const std::string& text, const char* prefix)
{
    return text.substr (std::string (prefix).size ());
}

} // namespace

bool IsTapirWriteCommand (const std::string& command)
{
    const std::string bare = StartsWith (command, TapirNamespace) ? After (command, TapirNamespace) : command;
    const auto* begin = std::begin (TapirWriteCommands);
    const auto* end = std::end (TapirWriteCommands);
    const auto* found = std::lower_bound (begin, end, bare, [] (const char* candidate, const std::string& value) {
        return std::string (candidate) < value;
    });
    return found != end && bare == *found;
}

bool IsTapirReadCommand (const std::string& command)
{
    const std::string bare = StartsWith (command, TapirNamespace) ? After (command, TapirNamespace) : command;
    // Tapir's Get* family is around a hundred commands and none of them writes.
    // Nothing else in its vocabulary is safe to infer from the name: Modify*,
    // Create*, Set* and Delete* obviously write, but so does Reserve*, and
    // SetViewSettings does not. Only the unambiguous half is claimed here.
    return StartsWith (bare, "Get");
}

CommandClass ClassifyCommand (const std::string& command)
{
    if (StartsWith (command, TapirNamespace)) {
        // ⚠️ THREE OUTCOMES, NOT TWO, AND THE THIRD IS THE IMPORTANT ONE. A
        // command is a write only when PROVEN, a read only when its name is
        // unambiguous, and unknown otherwise — because the write table misses
        // every command that opens its scope in a shared helper, and a new write
        // read as a read reports a run as clean while it costs a press per
        // element.
        if (IsTapirWriteCommand (command))
            return CommandClass::Write;
        return IsTapirReadCommand (command) ? CommandClass::Read : CommandClass::UnknownAddOn;
    }

    // Tapioca's own commands do not belong in this count. They go through the
    // dispatcher, which gives a batch ONE undo scope for the whole replay — that
    // is the entire difference this measurement exists to show, so folding them
    // in as one-step-each would erase it.
    if (StartsWith (command, TapiocaNamespace))
        return CommandClass::Read;

    // Official API.* commands: Archicad's own, and the ones Tapir uses are reads
    // (GetSelectedElements, GetProductInfo, IsAlive). A write through an official
    // command would be Archicad opening its own scope, which is out of scope for
    // a Tapir budget.
    if (StartsWith (command, OfficialNamespace))
        return CommandClass::Read;

    return CommandClass::Read;
}

UndoBudgetVerdict EvaluateUndoBudget (const std::vector<UndoLedgerEntry>& ledger)
{
    UndoBudgetVerdict verdict;
    std::vector<std::pair<uint32_t, std::string>> writes;

    for (const UndoLedgerEntry& entry : ledger) {
        if (entry.invocations == 0)
            continue;

        switch (ClassifyCommand (entry.command)) {
            case CommandClass::Write:
                verdict.actualSteps += entry.invocations;
                // One per DISTINCT command, however many times it was called:
                // that is the "one step per element type or modification" rule.
                verdict.idealSteps += 1;
                writes.emplace_back (entry.invocations, entry.command);
                break;
            case CommandClass::UnknownAddOn:
                verdict.unknownWrites += entry.invocations;
                break;
            case CommandClass::Read:
                verdict.readInvocations += entry.invocations;
                break;
        }
    }

    verdict.wastedSteps = verdict.actualSteps - verdict.idealSteps;
    // An unknown add-on command might be a write, so a run carrying one cannot
    // be declared within budget however tidy the rest of it looks.
    verdict.withinBudget = verdict.wastedSteps == 0 && verdict.unknownWrites == 0;

    // Worst first, and only the ones that actually cost more than their share —
    // a command called once is not an offender and listing it is noise.
    std::sort (writes.begin (), writes.end (),
               [] (const std::pair<uint32_t, std::string>& left, const std::pair<uint32_t, std::string>& right) {
                   if (left.first != right.first)
                       return left.first > right.first;
                   return left.second < right.second;
               });
    for (const auto& write : writes) {
        if (write.first > 1)
            verdict.offenders.push_back (write.second + " x" + std::to_string (write.first));
    }

    return verdict;
}

std::string DescribeUndoBudget (const UndoBudgetVerdict& verdict)
{
    // ⚠️ "SINCE THE LAST RUN", NOT "THIS RUN", AND THE DIFFERENCE IS NOT
    // PEDANTRY. Tapir's write components fire from their own button rather than
    // from a solve, so most of what a loop writes happens between runs. The
    // window that catches them is the honest one to name.
    if (verdict.actualSteps == 0 && verdict.unknownWrites == 0) {
        return "No Archicad writes since the last run, so no undo steps. (" + std::to_string (verdict.readInvocations) +
               " read(s).)";
    }

    std::string text = "Since the last run: " + std::to_string (verdict.actualSteps) + " undo step(s); " +
                       std::to_string (verdict.idealSteps) + " would do.";

    if (verdict.withinBudget) {
        text += "\nOne step per modification, which is the target.";
        return text;
    }

    if (verdict.wastedSteps > 0) {
        text += "\n" + std::to_string (verdict.wastedSteps) +
                " more press(es) than the work needed. Every Tapir write command opens its own undo "
                "scope, so a component called once per element costs one step per element.";
        if (!verdict.offenders.empty ()) {
            text += "\nCalled repeatedly:";
            for (const std::string& offender : verdict.offenders)
                text += "\n  " + offender;
        }
        text += "\nThe fix is to pass the whole list to one call rather than looping the component, or to "
                "route the write through Tapioca, where a batch replays inside a single undo scope.";
    }

    if (verdict.unknownWrites > 0) {
        text += "\n" + std::to_string (verdict.unknownWrites) +
                " call(s) went to Tapir commands that are not provably reads, and they are counted as "
                "possible writes. Element creation is the common case: those commands open their undo "
                "scope in a shared helper, so the table cannot see them. Treat this run's real cost as "
                "somewhere between the two numbers above.";
    }

    return text;
}

} // namespace grasshopper
} // namespace evp
