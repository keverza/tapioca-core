// ⚠️ BOUND BY OVERLAY-INVARIANTS.md -- sixty live runs bought those findings
// and each cost at least one. Composition stays at Present, a resize rebinds
// rather than relearns, and no production path may depend on a diagnostic.
// ArchViz/OverlayHudFollow -- the HUD pages' requests, drained on the message loop. Moved out
// of OverlayControllerHud.cpp::FollowHudState unchanged (#201) so the Tapioca viewer's own
// host, which has no overlay controller, drains the same requests when it shows the Massing
// workflow. MAIN THREAD: it reads and writes the model through the owning models.
#include "ArchViz/OverlayHudModel.hpp"
#include "ArchViz/OverlayGuestText.hpp"
#include "ArchViz/OverlayInput.hpp"
#include "ArchViz/SelectionMetadata.hpp"
#include "ArchViz/MassingModel.hpp"
#include "ArchViz/MassingHybrid.hpp"
#include "ArchViz/MassingSlicesModel.hpp"
#include "ArchViz/MassingInspectionModel.hpp"
#include "ArchViz/MassingBake.hpp"
#include "ArchViz/TextPrompt.hpp"
#include "ArchViz/HudConsole.hpp"
#include "ArchViz/HudFloorPlanFiles.hpp"
#include "Palette/ProgrammeDialog.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace overlayhudmodel {

bool FollowWorkflowRequests (const std::shared_ptr<overlayhud::State>& state)
{
    for (const auto& request : overlayhud::TakeMassingRequests (*state))
        massingmodel::Request (request);
    for (auto& edit : overlayhud::TakeMassingRuleEdits (*state))
        massingmodel::RequestRules (std::move (edit));
    for (auto& request : overlayhud::TakeMassingCalculations (*state))
        massinghybrid::Request (std::move (request));
    massinghybrid::Dimensions (overlayhud::MassingDimensions (*state));
    massingslicesmodel::HoverFunction (overlayhud::MassingStatsFunction (*state));
    massingslicesmodel::CollapseZone (overlayhud::MassingCollapseZone (*state));
    massinginspectionmodel::Follow (overlayhud::UniqueBuildings (*state), overlayhud::HighlightedBuilding (*state),
                                    overlayhud::BuildingFloorKey (*state), overlayhud::BuildingFloors (*state),
                                    overlayhud::MarkLargeFloors (*state), overlayhud::MassingCoefficients (*state),
                                    overlayhud::ShowLowHeadroom (*state));
    for (const auto& edit : overlayhud::TakeMassingCoefficientNumbers (*state)) {
        std::string answer;
        double number = 0;
        if (textprompt::Ask (edit.key + " (" + hudmeta::NumberText (edit.min) + " - " + hudmeta::NumberText (edit.max) +
                                 ")",
                             hudmeta::NumberText (edit.number), answer)) {
            if (!hudmeta::ParseNumber (answer, edit.min, edit.max, number))
                hudconsole::Warning ("Area coefficients", "Enter a finite number within the displayed bounds.");
            else if (!overlayhud::AnswerMassingCoefficientNumber (*state, edit, number))
                hudconsole::Warning ("Area coefficients",
                                     "Settings changed while the prompt was open; value not applied.");
        }
        overlayinput::RequestLayout (overlayinput::View::ThreeD);
        overlayinput::RequestLayout (overlayinput::View::Plan);
    }
    for (const auto& edit : overlayhud::TakeMassingNumbers (*state)) {
        std::string answer;
        double number = 0;
        if (textprompt::Ask (edit.key + " (" + hudmeta::NumberText (edit.min) + " - " + hudmeta::NumberText (edit.max) +
                                 ")",
                             hudmeta::NumberText (edit.number), answer)) {
            if (!hudmeta::ParseNumber (answer, edit.min, edit.max, number))
                hudconsole::Warning ("Massing input", "Enter a finite number within the displayed bounds.");
            else if (!overlayhud::AnswerMassingNumber (*state, edit, number))
                hudconsole::Warning ("Massing input", "Inputs changed while the panel was open; value not applied.");
        }
        overlayinput::RequestLayout (overlayinput::View::ThreeD);
        overlayinput::RequestLayout (overlayinput::View::Plan);
    }
    for (auto& request : overlayhud::TakeMassingCalculations (*state))
        massinghybrid::Request (std::move (request));
    // Define Programme's typed rows: the same native prompt as the numeric Set panels.
    for (const auto& edit : overlayhud::TakeProgrammeTexts (*state)) {
        std::string answer, error;
        if (textprompt::Ask (edit.type < 0 ? "Programme: share% rooms room min-max m2, types separated by ;"
                                           : "Flat type: share% rooms room min-max m2",
                             edit.text, answer) &&
            !overlayhud::AnswerProgrammeText (*state, edit, answer, error))
            hudconsole::Warning ("Programme", error);
        overlayinput::RequestLayout (overlayinput::View::ThreeD);
        overlayinput::RequestLayout (overlayinput::View::Plan);
    }
    for (const auto& edit : overlayhud::TakeProgrammeTables (*state)) {
        const auto epoch = massingmodel::ProjectEpoch ();
        const auto saved = massingmodel::Read ();
        floorprogramme::Programme answer;
        const bool accepted = evp::programmeui::Ask (edit, answer);
        // A modal pumps the host loop, even on Cancel. Never drain the old HUD's remaining
        // saves or actions into a different project after it returns.
        if (epoch != massingmodel::ProjectEpoch () || state != guesttext::HudState ()) {
            hudconsole::Warning ("Programme", "The project changed while the table was open; not applied.");
            return false;
        }
        if (accepted) {
            const auto current = massingmodel::Read ();
            std::string error;
            if (!current.known || !(saved.programme == current.programme))
                hudconsole::Warning ("Programme", "The project or saved programme changed; table not applied.");
            else if (!overlayhud::AnswerProgrammeTable (*state, edit, answer, error))
                hudconsole::Warning ("Programme", error);
        }
        overlayinput::RequestLayout (overlayinput::View::ThreeD);
        overlayinput::RequestLayout (overlayinput::View::Plan);
    }
    for (const auto& edit : overlayhud::TakeProgrammeColours (*state)) {
        const auto epoch = massingmodel::ProjectEpoch ();
        const auto saved = massingmodel::Read ();
        uint32_t rgba = 0;
        const bool accepted = evp::programmeui::AskColour (edit, rgba);
        if (epoch != massingmodel::ProjectEpoch () || state != guesttext::HudState ()) {
            hudconsole::Warning ("Programme", "The project changed while the colour picker was open; not applied.");
            return false;
        }
        if (accepted) {
            const auto current = massingmodel::Read ();
            std::string error;
            if (!current.known || !(saved.programme == current.programme))
                hudconsole::Warning ("Programme", "The project or saved programme changed; colour not applied.");
            else if (!overlayhud::AnswerProgrammeColour (*state, edit, rgba, error))
                hudconsole::Warning ("Programme", error);
        }
        overlayinput::RequestLayout (overlayinput::View::ThreeD);
        overlayinput::RequestLayout (overlayinput::View::Plan);
    }
    // A finished programme edit is stored in the project; Plan view exports become files.
    floorprogramme::Programme programme;
    if (overlayhud::TakeProgrammeSave (*state, programme))
        massingmodel::RequestProgramme (std::move (programme));
    for (const auto& file : overlayhud::TakePlanExports (*state))
        planfiles::Write (file);
    // The Selection page's metadata edits: written from the message loop too, in one undo step,
    // to every element selected then; the page is read again after (SelectionMetadata.hpp).
    std::vector<hudmeta::Edit> edits = overlayhud::TakeMetadataEdits (*state);
    std::vector<hudmeta::Edit> targeted, selected;
    std::map<std::string, std::vector<hudmeta::Edit>> buildingEdits;
    std::vector<std::string> targets;
    for (auto& edit : edits)
        if (!edit.expectedBuildingKey.empty ())
            buildingEdits[edit.expectedBuildingKey].push_back (std::move (edit));
        else if (edit.element.empty ())
            selected.push_back (std::move (edit));
        else {
            targets.push_back (edit.element);
            targeted.push_back (std::move (edit));
        }
    if (!selected.empty ())
        selectionmetadata::Request (std::move (selected), selectionmetadata::SelectedGuids (),
                                    [] () { overlayhudmodel::SelectionChanged (); });
    if (!targeted.empty ()) {
        std::sort (targets.begin (), targets.end ());
        targets.erase (std::unique (targets.begin (), targets.end ()), targets.end ());
        selectionmetadata::Request (std::move (targeted), std::move (targets),
                                    [] () { overlayhudmodel::SelectionChanged (); });
    }
    for (auto& [building, assigned] : buildingEdits) {
        std::vector<std::string> members;
        for (const auto& edit : assigned)
            members.push_back (edit.element);
        selectionmetadata::Request (std::move (assigned), std::move (members),
                                    [] () { overlayhudmodel::SelectionChanged (); });
    }
    // Automatic function-coloured massing slices replace the legacy accent-only highlight.
    // Settings' displays, as the user set them: switched, or a new look on what was cut.
    overlayhud::Displays displays;
    if (overlayhud::TakeDisplays (*state, displays))
        overlayhudmodel::ApplyDisplays (displays);
    for (const auto kind : overlayhud::TakeMassingBakes (*state))
        massingbake::Request (kind);
    return true;
}

} // namespace overlayhudmodel
} // namespace archviz
} // namespace geomsrv
