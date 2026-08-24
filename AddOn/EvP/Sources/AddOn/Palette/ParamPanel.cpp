#include "APIEnvir.h"
#include "ACAPinc.h"

#include "Palette/ParamPanel.hpp"
#include "ResourceIds.hpp"
#include "Palette/ParamLayout.hpp"
#include "Palette/PaletteMetrics.hpp"
#include "Palette/PaletteScroll.hpp"        // F4 — every row reaches the panel through it
#include "Palette/ParamVisibility.hpp"      // F3 show_when — DevKit-free, tested offline
#include "Palette/ParamValues.hpp"          // a control's value <-> its text, both ways
#include "Palette/NavItemChoices.hpp"       // evp.View / evp.Database — the rows and their guids
#include "Palette/NavigatorBrowser.hpp"     // evp.View — the modal Navigator tree
#include "Palette/CatalogPicker.hpp"        // evp.LibraryPart / evp.Favourite — catalogue + modal
#include "Palette/AttributePickerTypes.hpp" // UserControlTypeFor — evp.Layer/… -> Archicad's picker
#include "ControlPalette.hpp"               // evp::CommandInfo + the shell, the sole observer
#include "NativeCommands/CommandUtils.hpp"  // geomsrv::AttributeNameToIndex / …IndexToName
#include "AddOnCommands.hpp"                // geomsrv::ProjectInfoFieldChoices — evp.ProjectField rows
#include "Python/PathUtils.hpp"

#include "ObjectState.hpp"
#include "ObjectStateJSONConversion.hpp"

#include "DGFileDialog.hpp"    // evp.FilePath Browse button
#include "FileTypeManager.hpp" // FTM::RootGroup — the "any file" filter
#include "Location.hpp"        // IO::Location -> path string

#include <algorithm>
#include <string>

using namespace evp::palette;

namespace {

constexpr short LabelGap = 8;
constexpr short DomainHintWidth = 84;

constexpr short PenSwatchWidth = 33;    // as in DG_Test's "Line Pen Setting" (33x19)
constexpr short BrowseButtonWidth = 24; // compact folder icon before a FilePath field
constexpr short BrowseButtonGap = 4;

std::string NormalizedFileExtension (const GS::UniString& rawExtension)
{
    const char* const rawText = rawExtension.ToCStr ();
    if (rawText == nullptr)
        return {};

    std::string extension (rawText);
    if (!extension.empty () && extension.front () == '.')
        extension.erase (0, 1);
    return extension;
}

// UserControlTypeFor — which Archicad picker an evp.<Attribute> type maps to —
// lives in Palette/AttributePickerTypes.hpp. It is a table of claims about the
// picker's supported-type list, not panel layout, and it left this file when the
// soft cap forced a split.

} // namespace

namespace evp {

// ---------------------------------------------------------------------------
ParamPanel::ParamPanel (const DG::Panel& panel, ControlPalette& observer,
                        std::vector<std::unique_ptr<DG::UserControl>>& penPool)
    : panel (panel), observer (observer), penPool (penPool)
{
}

void ParamPanel::Create ()
{
    // One section rule, BELOW the command list, between the required and the
    // optional rows. Nothing above the list gets a rule — the server row and the
    // list already read as separate things, and rules there were just noise.
    // Hidden until a command has rows on both sides of it.
    groupRule = std::make_unique<DG::Separator> (panel, DG::Rect (Margin, 0, Margin + 100, 2));
    groupRule->Hide ();

    // F3 — the rule under the pinned Action row. Same lifetime, same reason for
    // being built here: DG item creation stays in one place.
    actionRule = std::make_unique<DG::Separator> (panel, DG::Rect (Margin, 0, Margin + 100, 2));
    actionRule->Hide ();
}

void ParamPanel::Clear ()
{
    paramControls.clear (); // unique_ptrs destroy the DG items

    // The pool is NOT owned by the rows, so nothing above hid these. Left visible,
    // a swatch from the previous command would linger over the new one's layout.
    // Also re-enable: a readonly pen param disables its borrowed swatch, and the
    // pool is reused, so without this the disabled state would leak to the next
    // command that claims the same swatch.
    for (std::unique_ptr<DG::UserControl>& swatch : penPool) {
        swatch->Hide ();
        swatch->Enable ();
    }
}

// The dialog generator: scanned metadata -> DG controls, built at runtime.
void ParamPanel::Rebuild (const CommandInfo& info)
{
    Clear ();

    // The description is NOT here any more: it is its own band above this one
    // (Palette/DescriptionPanel), so it can be folded away and dragged. It used
    // to open this block, always fully expanded, with no way to put it away.

    // Positions are assigned by PlaceAt; this rect is only a placeholder.
    const DG::Rect seed (Margin, 0, Margin + 100, RowHeight);

    size_t nextPenSlot = 0; // into penPool

    for (const GS::UniString& paramJson : info.paramJsons) {
        GS::ObjectState os;
        if (JSON::ConvertToObjectState (paramJson, os) != NoError)
            continue;

        ParamControl pc;
        API_UserControlType controlType = APIUserControlType_Layer; // set by UserControlTypeFor
        GS::UniString domainText;                                   // empty unless the param declares bounds
        os.Get ("name", pc.name);
        // No default in run()'s signature -> required. The scanner reports it, so
        // this stays in step with the code rather than a hand-kept list.
        bool requiredFlag = false;
        if (os.Get ("required", requiredFlag))
            pc.required = requiredFlag;
        os.Get ("type", pc.type);
        os.Get ("unit", pc.unit);
        os.Get ("extensions", pc.fileExtensions);
        os.Get ("mode", pc.fileMode);

        // Per-param attributes ride in the param's own flat dict (the scanner folds
        // arbitrary annotation kwargs in — evp.Float(..., readonly=True,
        // default_from="project:Sklypo plotas")). readonly shows-but-locks the
        // control; default_from prefills it from the project (see the Float branch).
        bool readonly = false;
        os.Get ("readonly", readonly);
        GS::UniString defaultFrom;
        const bool haveDefaultFrom = os.Get ("default_from", defaultFrom);
        bool defaultFromResolved = false; // set below if default_from resolves

        // F3 — show_when, already FLAT: the scanner turned {"action": [...]} into a
        // name plus a string array, because a nested dict is the one shape this
        // side cannot read back (it would silently arrive as nothing at all — a
        // control that is always visible, with no error anywhere).
        os.Get ("show_when_param", pc.showWhenParam);
        os.Get ("show_when_values", pc.showWhenValues);

        // The row label is the parameter's display text when the command declares
        // one (@evp.command(labels={...}), folded per-param by the scanner), else
        // the parameter name itself. The name is an ASCII identifier; a label can
        // carry spaces and diacritics, so a command can present inputs in the user's
        // language. JSON strings arrive already Unicode-decoded — no .grc codepage.
        pc.label = std::make_unique<DG::LeftText> (panel, seed);
        GS::UniString labelText;
        if (!os.Get ("label", labelText) || labelText.IsEmpty ())
            labelText = pc.name;
        const GS::UniString projectUnit = ProjectUnitLabel (pc.unit);
        if (!projectUnit.IsEmpty ())
            labelText += " (" + projectUnit + ")";
        pc.label->SetText (labelText);

        if (pc.type == "bool" || pc.type == "Bool") {
            // evp.Bool is a bare `bool` with room for kwargs — same control, so a
            // command can put show_when/readonly on a checkbox without the
            // language having anywhere to hang them.
            pc.kind = ParamControl::Kind::Bool;
            auto box = std::make_unique<DG::CheckBox> (panel, seed);
            bool value = false;
            os.Get ("default", value);
            box->SetText ("");
            if (value)
                box->Check ();
            // A checkbox can CONTROL a show_when, so its changes have to reach
            // ApplyVisibility. Attached to the shell, never to this panel.
            box->Attach (observer);
            pc.control = std::move (box);
        }
        else if (pc.type == "Int" || pc.type == "int") {
            pc.kind = ParamControl::Kind::Int;
            auto edit = std::make_unique<DG::IntEdit> (panel, seed);
            GS::Int32 minimum = 0, maximum = 0, value = 0;
            const bool haveMin = os.Get ("minimum", minimum);
            const bool haveMax = os.Get ("maximum", maximum);
            if (haveMin)
                edit->SetMin (minimum);
            if (haveMax)
                edit->SetMax (maximum);
            os.Get ("default", value);
            edit->SetValue (value);
            pc.control = std::move (edit);

            domainText = FormatDomain (haveMin, haveMax, GS::UniString::Printf ("%d", (int) minimum),
                                       GS::UniString::Printf ("%d", (int) maximum));
        }
        else if (pc.type == "Float" || pc.type == "float") {
            double value = 0.0;
            os.Get ("default", value);

            // default_from="project:<field>" prefills from a project-info field at
            // dialog-build (same source the Python reads), overriding the literal
            // default when it resolves — typically paired with readonly for a field
            // like plot area that comes from File > Info > Project Info.
            if (haveDefaultFrom) {
                double resolved = 0.0;
                if (ResolveDefaultFromNumber (defaultFrom, resolved)) {
                    value = resolved;
                    defaultFromResolved = true;
                }
            }
            // DG formats API values in project Working Units; convert annotation
            // defaults/bounds in and collected values back for Python normalization.
            std::unique_ptr<DG::RealEdit> edit;
            if (pc.unit == "m") {
                pc.kind = ParamControl::Kind::Length;
                edit = std::make_unique<DG::LengthEdit> (panel, seed);
            }
            else if (pc.unit == "m2") {
                pc.kind = ParamControl::Kind::Area;
                edit = std::make_unique<DG::AreaEdit> (panel, seed);
            }
            else if (pc.unit == "m3") {
                pc.kind = ParamControl::Kind::Volume;
                edit = std::make_unique<DG::VolumeEdit> (panel, seed);
            }
            else if (pc.unit == "rad") {
                pc.kind = ParamControl::Kind::Angle;
                edit = std::make_unique<DG::AngleEdit> (panel, seed);
            }
            else {
                pc.kind = ParamControl::Kind::Real;
                edit = std::make_unique<DG::RealEdit> (panel, seed);
            }
            double minimum = 0.0, maximum = 0.0;
            const bool haveMin = os.Get ("minimum", minimum);
            const bool haveMax = os.Get ("maximum", maximum);
            if (haveMin)
                edit->SetMin (minimum);
            if (haveMax)
                edit->SetMax (maximum);
            edit->SetValue (value);
            pc.control = std::move (edit);

            domainText = FormatNumericDomain (pc.unit, haveMin, haveMax, minimum, maximum);
        }
        else if (pc.type == "Pen" && nextPenSlot < penPool.size ()) {
            // Archicad's real pen swatch, borrowed from the .grc pool. The attribute
            // picker CANNOT do this: APIUserControlType_Pen is absent from the types
            // API_AttributePickerParams documents, and asking for it just fails.
            // Pens are not attributes at all — they are numbers 1..255 — so the
            // value here is the pen index, not a name.
            pc.kind = ParamControl::Kind::Pen;
            pc.penSwatch = penPool[nextPenSlot++].get ();

            GS::Int32 pen = 1;
            os.Get ("default", pen);
            pc.penSwatch->SetValue (pen);
        }
        else if (pc.type == "Pen") {
            // Pool exhausted: more pen parameters than reserved .grc items.
            pc.kind = ParamControl::Kind::Int;
            auto edit = std::make_unique<DG::IntEdit> (panel, seed);
            GS::Int32 pen = 1;
            os.Get ("default", pen);
            edit->SetMin (1);
            edit->SetMax (255);
            edit->SetValue (pen);
            pc.control = std::move (edit);
            domainText = FormatDomain (true, true, "1", "255");
            AppendTextLine (
                ScanLogPath (),
                GS::UniString::Printf ("  pen '%T': the pen swatch pool holds only %u item(s) and they are all "
                                       "claimed - using a 1..255 number field. Raise PenPoolSize in "
                                       "ResourceIds.hpp and add matching UserControl items to AddOn.grc.",
                                       pc.name.ToPrintf (), (unsigned) penPool.size ()));
        }
        else if (UserControlTypeFor (pc.type, controlType, pc.attrType)) {
            // Archicad's OWN attribute picker: a PushCheck showing the current
            // attribute plus an arrow, which opens the project's real chooser. The
            // user cannot type here, so a typo can never invent a layer.
            auto host = std::make_unique<DG::PushCheck> (panel, seed);

            API_AttributePickerParams params;
            params.type = controlType;
            params.dialogID = panel.GetId ();
            params.itemID = host->GetId ();
            params.pushCheckAppearance = API_AttributePickerParams::PushCheckAppearance::ArrowIconAndText;

            const bool created =
                (ACAPI_Dialog_CreateAttributePicker (params, pc.picker) == NoError) && (pc.picker != nullptr);

            if (created) {
                pc.kind = ParamControl::Kind::Attribute;
                GS::UniString name;
                API_AttributeIndex index;
                os.Get ("default", name);
                if (geomsrv::AttributeNameToIndex (pc.attrType, name, index))
                    pc.picker->SetSelectedAttributeIndex (index);
                pc.control = std::move (host);
            }
            else {
                // A popup listing what the project ACTUALLY contains. No typing, so
                // still no invented attributes — just not Archicad's own widget.
                host.reset ();
                pc.kind = ParamControl::Kind::Enum;
                auto popup = std::make_unique<DG::PopUp> (panel, seed, RowHeight, 0);

                GS::Array<API_Attribute> attributes;
                GS::UniString value;
                os.Get ("default", value);
                if (ACAPI_Attribute_GetAttributesByType (pc.attrType, attributes) == NoError) {
                    for (const API_Attribute& attribute : attributes)
                        pc.choices.Push (GS::UniString (attribute.header.name));
                }
                for (UIndex i = 0; i < pc.choices.GetSize (); ++i) {
                    popup->AppendItem ();
                    popup->SetItemText ((short) (i + 1), pc.choices[i]);
                    if (pc.choices[i] == value)
                        popup->SelectItem ((short) (i + 1));
                }
                if (pc.choices.IsEmpty ()) {
                    popup->AppendItem ();
                    popup->SetItemText (1, "(no attributes in this project)");
                }
                pc.control = std::move (popup);
                AppendTextLine (ScanLogPath (), GS::UniString::Printf (
                                                    "  picker: ACAPI_Dialog_CreateAttributePicker REFUSED type %T - "
                                                    "listing %u project attribute(s) in a popup instead. The requested "
                                                    "API_UserControlType is probably not on the supported list in "
                                                    "API_AttributePickerParams.",
                                                    pc.type.ToPrintf (), (unsigned) pc.choices.GetSize ()));
            }
        }
        else if (pc.type == "Enum" || pc.type == "Action") {
            // Mechanically identical; the kind differs only so PlaceAt can pin the
            // Action above every other row (it is the command's mode, and the rows
            // below exist because of it).
            pc.kind = (pc.type == "Action") ? ParamControl::Kind::Action : ParamControl::Kind::Enum;
            // vSize = drop-down row height, textOffset = text indent.
            auto popup = std::make_unique<DG::PopUp> (panel, seed, RowHeight, 0);
            os.Get ("args", pc.choices);
            GS::UniString value;
            os.Get ("default", value);
            for (UIndex i = 0; i < pc.choices.GetSize (); ++i) {
                popup->AppendItem ();
                popup->SetItemText ((short) (i + 1), pc.choices[i]);
                if (pc.choices[i] == value)
                    popup->SelectItem ((short) (i + 1));
            }
            // An Action with no default in the signature would open on nothing,
            // and its show_when rows would all be hidden until the user noticed —
            // which reads as a command with no inputs. Fall back to the first mode.
            if (popup->GetSelectedItem () < 1 && !pc.choices.IsEmpty ())
                popup->SelectItem (1);
            // Both kinds can CONTROL a show_when, so both are observed. Before F3
            // no popup was attached to anything.
            popup->Attach (observer);
            pc.control = std::move (popup);
        }
        else if (pc.type == "Story") {
            // A popup of the project's real stories, so a story parameter cannot name
            // one that does not exist. Rows read "0  Ground floor"; the value handed
            // to run() is the story INDEX, which is NOT the row number — stories run
            // from firstStory, often negative. The index lives in pc.storyIndices,
            // parallel to the rows. Read straight from ACAPI here rather than round-
            // tripping EvP.GetStories: the palette is already on the main thread.
            pc.kind = ParamControl::Kind::Story;
            auto popup = std::make_unique<DG::PopUp> (panel, seed, RowHeight, 0);

            GS::Int32 defaultIndex = 0;
            const bool haveDefault = os.Get ("default", defaultIndex);

            API_StoryInfo storyInfo = {};
            short selectRow = 0;
            if (ACAPI_ProjectSetting_GetStorySettings (&storyInfo) == NoError && storyInfo.data != nullptr) {
                const short count = storyInfo.lastStory - storyInfo.firstStory + 1;
                for (short i = 0; i < count; ++i) {
                    const API_StoryType& story = (*storyInfo.data)[i];
                    popup->AppendItem ();
                    popup->SetItemText (
                        (short) (i + 1),
                        GS::UniString::Printf ("%d  %T", (int) story.index, GS::UniString (story.uName).ToPrintf ()));
                    pc.storyIndices.Push ((GS::Int32) story.index);
                    // Preselect the declared default index, or the active story when
                    // the parameter has no default of its own.
                    if (haveDefault ? (story.index == defaultIndex) : (story.index == storyInfo.actStory))
                        selectRow = (short) (i + 1);
                }
                // The SDK allocated this handle for us; free it.
                BMKillHandle (reinterpret_cast<GSHandle*> (&storyInfo.data));
            }
            if (pc.storyIndices.IsEmpty ()) {
                popup->AppendItem ();
                popup->SetItemText (1, "(no stories in this project)");
            }
            else {
                popup->SelectItem (selectRow >= 1 ? selectRow : (short) 1);
            }
            pc.control = std::move (popup);
        }
        else if (pc.type == "ProjectField") {
            // A popup of the open project's Project Info fields, so a command can
            // ask WHICH field a value comes from instead of hard-coding one in its
            // source. Read live here rather than through Tapioca.GetProjectInfo:
            // the palette is already on the main thread, and the fields must
            // reflect the project as it is when the row is built.
            //
            // ⚠️ THE ROW SHOWS THE DESCRIPTION AND THE VALUE IS THE KEY — the same
            // display-vs-identity split evp.View makes, and for the same reason. A
            // Project Info DESCRIPTION is user-authored display data and need not be
            // unique; the DATABASE KEY is the identity ACAPI_AutoText_SetAnAutoText
            // writes by. So the description goes on the row, where it is the only
            // readable thing, and the key goes to run(), where it is the only
            // dependable one.
            pc.kind = ParamControl::Kind::ProjectField;
            auto popup = std::make_unique<DG::PopUp> (panel, seed, RowHeight, 0);

            // numeric=True offers only the fields a NUMBER can be read out of.
            // Stock Project Info is mostly prose — company, client, addresses,
            // dates — so a picker feeding a Float parameter otherwise lists dozens
            // of rows of which two are usable, and picking a wrong one fails much
            // later, as a missing area rather than as a bad choice.
            //
            // ⚠️ The test is ParseLocalizedNumber, the SAME predicate default_from
            // resolves through (see ResolveDefaultFromNumber). Any other test here
            // — "looks like digits", a stricter parse — could offer a field that
            // then fails to prefill, or hide one that would have worked.
            bool numericOnly = false;
            os.Get ("numeric", numericOnly);

            GS::Array<GS::UniString> fieldDescriptions, fieldKeys, fieldValues;
            geomsrv::ProjectInfoFieldChoices (fieldDescriptions, fieldKeys, fieldValues);
            for (UIndex i = 0; i < fieldDescriptions.GetSize (); ++i) {
                if (numericOnly) {
                    double parsed = 0.0;
                    if (i >= fieldValues.GetSize () || !ParseLocalizedNumber (fieldValues[i], parsed))
                        continue;
                }
                pc.choices.Push (fieldDescriptions[i]);
                // Pushed TOGETHER, never in separate loops: the row and the key it
                // sends are only related by index (see ParamPanel.hpp).
                pc.choiceValues.Push (i < fieldKeys.GetSize () ? fieldKeys[i] : GS::UniString ());
            }

            // The declared default is matched the way ProjectInfoField matches:
            // key first and exactly, then description by substring. That is what
            // lets a command spell a readable `= "Sklypo plotas"` and still land on
            // a field the user labelled "Sklypo plotas, m2", while a source that
            // names the key hits it precisely.
            GS::UniString declared;
            os.Get ("default", declared);
            const GS::UniString needle = declared.ToLowerCase ();
            short selectRow = 0;
            for (UIndex i = 0; i < pc.choices.GetSize (); ++i) {
                popup->AppendItem ();
                popup->SetItemText ((short) (i + 1), pc.choices[i]);
                if (!needle.IsEmpty () && i < pc.choiceValues.GetSize () && pc.choiceValues[i].ToLowerCase () == needle)
                    selectRow = (short) (i + 1); // exact key wins outright
            }
            if (selectRow == 0 && !needle.IsEmpty ()) {
                for (UIndex i = 0; i < pc.choices.GetSize (); ++i) {
                    if (pc.choices[i].ToLowerCase ().Contains (needle)) {
                        selectRow = (short) (i + 1);
                        break; // first description match
                    }
                }
            }
            if (pc.choices.IsEmpty ()) {
                // Nothing is pushed to choices/choiceValues, so this placeholder
                // reads back as an empty key — which is what the command refuses
                // on, and what keeps it from satisfying a required parameter.
                popup->AppendItem ();
                popup->SetItemText (1, numericOnly ? "(no numeric project-info fields)" : "(no project-info fields)");
            }
            else {
                popup->SelectItem (selectRow >= 1 ? selectRow : (short) 1);
            }
            // A ProjectField can control a show_when like any other popup.
            popup->Attach (observer);
            pc.control = std::move (popup);
        }
        else if (pc.type == "View") {
            // A BUTTON that opens the Navigator browser, not a list. The value
            // handed to run() is the chosen item's GUID; the button shows the row
            // text so the choice is readable without opening anything.
            //
            // ⚠️ WHY THE VALUE IS A GUID AND NOT A NAME — the one picker that
            // breaks the names-not-indices policy, and it is measured, not a
            // preference: navigator names are NOT UNIQUE (12 views called "Story"
            // in one stock project), so a name identifies nothing. A guid does, but
            // a guid is invisible in Archicad's UI, so a user cannot type one
            // either. Show the name, send the guid.
            //
            // ⚠️ WHY A BROWSER AND NOT A POPUP. This WAS a flat popup of every
            // placeable view. On a real project the verdict was "using current
            // picker user would get lost on a larger project" — hundreds of rows
            // with no hierarchy. See Palette/NavigatorBrowser.hpp.
            pc.kind = ParamControl::Kind::NavBrowse;

            // A default is honoured only when it is a guid that still resolves.
            // Anything else — a name, a stale guid — is DROPPED rather than shown,
            // because a button reading a label whose item no longer exists is worse
            // than an empty one: it looks like a valid choice.
            GS::UniString declared;
            os.Get ("default", declared);
            if (LookUpViewLabel (declared, pc.selectedLabel))
                pc.selectedGuid = declared;

            auto button = std::make_unique<DG::Button> (panel, seed);
            button->SetText (pc.selectedLabel.IsEmpty () ? GS::UniString ("Navigator") : pc.selectedLabel);
            button->Attach (observer);
            pc.control = std::move (button);
        }
        else if (pc.type == "Database") {
            // A flat popup, deliberately: the independent databases are a dozen
            // entries with no hierarchy, so the browser dialog would be ceremony.
            // Rows read "layout  -  A-101 Plans"; the value is the database guid.
            pc.kind = ParamControl::Kind::NavItem;
            auto popup = std::make_unique<DG::PopUp> (panel, seed, RowHeight, 0);

            GS::Array<NavItemChoice> rows;
            CollectDatabaseChoices (rows);

            GS::UniString value;
            os.Get ("default", value);
            short selectRow = 0;

            for (UIndex i = 0; i < rows.GetSize (); ++i) {
                popup->AppendItem ();
                popup->SetItemText ((short) (i + 1), rows[i].label);
                pc.itemGuids.Push (rows[i].guid); // row and guid, one statement
                if (!value.IsEmpty () && selectRow == 0 && (rows[i].guid == value || rows[i].label == value))
                    selectRow = (short) (i + 1);
            }

            if (pc.itemGuids.IsEmpty ()) {
                // Nothing to choose. One placeholder row with NO guid behind it, so
                // read-back yields "" and the command's own validation reports it.
                // Never silently send row 1: that would be a wrong item, not a
                // missing one.
                popup->AppendItem ();
                popup->SetItemText (1, "(no databases in this project)");
            }
            else {
                popup->SelectItem (selectRow >= 1 ? selectRow : (short) 1);
            }

            popup->Attach (observer);
            pc.control = std::move (popup);
        }
        else if (pc.type == "LibraryPart" || pc.type == "Favourite") {
            // A BUTTON that opens the catalogue browser. Same shape as evp.View
            // and for the same recorded reason: a loaded library is thousands of
            // parts, and a flat popup of thousands is a list you scroll, not one
            // you choose from.
            //
            // ⚠️ NOTHING IS ENUMERATED HERE. Rebuild() runs every time the user
            // merely SELECTS a command in the palette's list, and walking the
            // whole library on each of those — for a picker that may never be
            // opened — would make choosing a command feel broken. The catalogue
            // is collected on the first click; see HandleButtonClicked.
            //
            // ⚠️ A DECLARED DEFAULT IS DROPPED. `= None` is the only sensible
            // default these can carry: a name is not an identity (two libraries
            // ship the same part; the newest one wins and the other is invisible)
            // so a default naming one would be a button that looks chosen and may
            // be pointing at something else. Required-ness is what gates Run.
            pc.kind = ParamControl::Kind::Catalog;
            os.Get (pc.type == "Favourite" ? "element_type" : "subtype", pc.catalogFilter);

            auto button = std::make_unique<DG::Button> (panel, seed);
            button->SetText (pc.type == "Favourite" ? GS::UniString ("Choose favourite")
                                                    : GS::UniString ("Choose library object"));
            button->Attach (observer);
            pc.control = std::move (button);
        }
        else if (pc.type == "FilePath") {
            // A text field you can type into PLUS a Browse button, because
            // remembering a full path is not the user's job. The value is the path
            // string; the button just fills the field (see HandleButtonClicked).
            pc.kind = ParamControl::Kind::FilePath;
            auto edit = std::make_unique<DG::TextEdit> (panel, seed);
            GS::UniString value;
            os.Get ("default", value);
            edit->SetText (value);
            edit->Attach (observer); // typing re-checks the Run gate, as for Text
            pc.control = std::move (edit);

            pc.browseButton = std::make_unique<DG::IconButton> (panel, seed);
            pc.browseButton->SetIcon (DG::Icon (ACAPI_GetOwnResModule (), PaletteIconFolderId));
            pc.browseButton->Attach (observer);
        }
        else {
            // str, evp.Layer and anything unrecognised fall back to a text field.
            // Layer becomes an attribute picker above; a text field is honest here.
            pc.kind = ParamControl::Kind::Text;
            auto edit = std::make_unique<DG::TextEdit> (panel, seed);
            GS::UniString value;
            os.Get ("default", value);
            edit->SetText (value);
            // Observed so typing re-checks the Run gate immediately — a REQUIRED
            // text field starts empty, so without this Run stays disabled while
            // the user types and the parameter looks ignored.
            edit->Attach (observer);
            pc.control = std::move (edit);
        }

        // A bounded number states its domain beside the field. DG silently clamps a
        // value outside SetMin/SetMax, so without this the field just "refuses to
        // type" with no explanation.
        if (!domainText.IsEmpty ()) {
            pc.domainHint = std::make_unique<DG::LeftText> (panel, seed);
            pc.domainHint->SetText (domainText);
        }

        // Pen swatches are pool items, attached once in the shell's constructor.
        if (pc.kind == ParamControl::Kind::Attribute)
            static_cast<DG::PushCheck*> (pc.control.get ())->Attach (observer);

        // readonly: shown but locked, so the value can only be its default /
        // default_from. Disable the widget (and any Browse button). A disabled field
        // the user cannot fill must NEVER gate Run — WhatIsMissing would deadlock on
        // a required-but-empty one — so drop `required` unless default_from already
        // placed a usable value in it.
        if (readonly) {
            pc.Widget ()->Disable ();
            if (pc.browseButton)
                pc.browseButton->Disable ();
            if (!defaultFromResolved)
                pc.required = false;
        }

        // NOT shown yet — the shell lays out first, then calls ShowControls.
        paramControls.push_back (std::move (pc));
    }

    // Every row exists now, so the show_whens can finally be resolved against each
    // other. Done here rather than lazily: the shell's very first PlaceAt must
    // already know which rows take height.
    ApplyVisibility ();
}

// F3 — the show_whens against the values the controls hold right now. The rule
// evaluation is Palette/ParamVisibility's, on purpose: it is the half of this
// feature that can be wrong invisibly, so it lives where a test can reach it.
bool ParamPanel::ApplyVisibility ()
{
    std::vector<std::string> names, values;
    std::vector<VisibilityRule> rules;
    names.reserve (paramControls.size ());
    values.reserve (paramControls.size ());
    rules.reserve (paramControls.size ());

    for (const ParamControl& pc : paramControls) {
        names.push_back (Utf8 (pc.name));
        values.push_back (Utf8 (pc.CurrentValueText ()));
        VisibilityRule rule;
        rule.controller = Utf8 (pc.showWhenParam);
        for (const GS::UniString& value : pc.showWhenValues)
            rule.values.push_back (Utf8 (value));
        rules.push_back (std::move (rule));
    }

    const std::vector<bool> visible = EvaluateVisibility (names, values, rules);

    bool changed = false;
    for (size_t i = 0; i < paramControls.size (); ++i) {
        if (paramControls[i].visible != visible[i]) {
            paramControls[i].visible = visible[i];
            changed = true;
        }
    }
    return changed;
}

// Reveal everything AFTER the shell has positioned it. A dynamically created DG item
// starts hidden at its construction rect; showing it first and moving it afterwards
// makes every control flash in the panel's top-left corner on the way to its real
// place. Lay out while hidden, then show.
void ParamPanel::ShowControls ()
{
    // The description lines are NOT touched here: PlaceAt puts them on the panel
    // through the scroll, which already showed or hid each one according to whether
    // it fits the viewport (F4). Re-showing them here would put a scrolled-away line
    // straight back on top of the fixed header.

    // Show/HIDE, not just show: this is also what a reflow calls after a show_when
    // changed, and a row that just became invisible has to be taken off the panel.
    // PlaceAt gave it no rect, so leaving it shown would leave it wherever the
    // previous layout put it, on top of whatever is there now. `clipped` is the same
    // question asked by the scroll rather than by show_when.
    for (ParamControl& pc : paramControls) {
        const bool on = pc.visible && !pc.clipped;
        if (on) {
            pc.label->Show ();
            pc.Widget ()->Show ();
        }
        else {
            pc.label->Hide ();
            pc.Widget ()->Hide ();
        }
        if (pc.domainHint) {
            if (on)
                pc.domainHint->Show ();
            else
                pc.domainHint->Hide ();
        }
        if (pc.browseButton) {
            if (on)
                pc.browseButton->Show ();
            else
                pc.browseButton->Hide ();
        }
    }
}

// Generated controls flow down the band, grouped required-then-optional so the
// things that BLOCK Run are read first.
short ParamPanel::PlaceAt (short top, short left, short right, const PaletteScroll& clip)
{
    short y = top;

    const auto placeRow = [&] (ParamControl& pc) {
        // F13.A — fields are a compact right-aligned column. At the default
        // palette they take roughly a third of the content; a wider palette
        // gives almost all new room to labels. FilePath uses that same assembly
        // width, with its compact Browse icon first and its text field second.
        const short inputWidth = (short) InputColumnWidth (right - left);
        const short assemblyLeft = (short) (right - inputWidth);
        short controlLeft = pc.kind == ParamControl::Kind::FilePath
                                ? (short) (assemblyLeft + BrowseButtonWidth + BrowseButtonGap)
                                : assemblyLeft;
        short controlRight = right;

        // A pen swatch is a fixed-size colour chip in Archicad's own dialogs,
        // not a field: stretching it would look nothing like the native UI.
        if (pc.kind == ParamControl::Kind::Pen)
            controlRight = (short) (controlLeft + PenSwatchWidth);

        // The domain hint stays immediately left of the field. Labels now use
        // every remaining pixel and naturally truncate first on a narrow panel.
        const short hintRight = (short) (controlLeft - 4);
        const short hintLeft = (short) (hintRight - DomainHintWidth);
        const short labelRight = pc.domainHint ? (short) (hintLeft - LabelGap) : (short) (assemblyLeft - LabelGap);

        // F4 — the whole row stands or falls together: if the scrolled column has
        // moved any of it out of the viewport, none of it goes on the panel, and
        // ShowControls is told so it cannot put it back.
        pc.clipped = !clip.IsVisible (y, (short) (y + RowHeight));

        clip.Place (pc.label.get (), DG::Rect (left, y + 3, labelRight, y + RowHeight));
        clip.Place (pc.domainHint.get (), DG::Rect (hintLeft, y + 3, hintRight, y + RowHeight));
        clip.Place (pc.Widget (), DG::Rect (controlLeft, y, controlRight, y + RowHeight));
        clip.Place (pc.browseButton.get (),
                    DG::Rect (assemblyLeft, y, (short) (assemblyLeft + BrowseButtonWidth), y + RowHeight));
        y += RowHeight + RowGap;
    };

    // A group of rows, or false when it has none to place. INVISIBLE ROWS ARE
    // SKIPPED ENTIRELY (F3): a hidden row takes no height, so switching the action
    // shortens the block rather than leaving a gap where its inputs used to be.
    const auto placeGroup = [&] (auto&& wanted) {
        bool any = false;
        for (ParamControl& pc : paramControls) {
            if (!pc.visible || !wanted (pc))
                continue;
            placeRow (pc);
            any = true;
        }
        if (any)
            y += 4;
        return any;
    };

    const auto isAction = [] (const ParamControl& pc) { return pc.kind == ParamControl::Kind::Action; };

    // A rule this layout did not use must be HIDDEN, or it lingers wherever the
    // previous command left it — a .grc-free item keeps its last rect.
    const auto placeRule = [&] (std::unique_ptr<DG::Separator>& rule, bool wanted) {
        if (!rule)
            return;
        if (!wanted) {
            rule->Hide ();
            return;
        }
        clip.Place (rule.get (), DG::Rect (left, y, right, (short) (y + 2)));
        y += 10;
    };

    // F3 — the Action is pinned above everything, with its own rule under it, so
    // the mode reads as the thing the inputs answer to rather than as the first
    // of them.
    placeRule (actionRule, placeGroup (isAction));

    // Then required, the grouping rule, and optional — the things that BLOCK Run
    // read first.
    const bool hadRequired = placeGroup ([&] (const ParamControl& pc) { return !isAction (pc) && pc.required; });
    const bool hasOptional = std::any_of (paramControls.begin (), paramControls.end (), [&] (const ParamControl& pc) {
        return pc.visible && !isAction (pc) && !pc.required;
    });
    placeRule (groupRule, hadRequired && hasOptional);
    placeGroup ([&] (const ParamControl& pc) { return !isAction (pc) && !pc.required; });

    return (short) (y - top);
}

// A checkbox or an attribute picker's PushCheck host. The picker hands over to
// Archicad's own chooser; a checkbox needs nothing done to it, but it may CONTROL
// a show_when, so both re-evaluate visibility on the way out.
bool ParamPanel::HandleCheckItemChanged (const DG::CheckItemChangeEvent& ev, bool& reflow)
{
    for (ParamControl& pc : paramControls) {
        if (ev.GetSource () != pc.control.get ())
            continue;
        if (pc.kind == ParamControl::Kind::Attribute) {
            if (pc.picker != nullptr)
                pc.picker->Invoke (); // updates the control's own text
            // PushCheck latches when clicked; the picker is not a toggle.
            static_cast<DG::PushCheck*> (pc.control.get ())->Uncheck ();
        }
        reflow = ApplyVisibility ();
        return true;
    }
    return false;
}

// A generated popup — the Action, or any Enum. Selecting a mode is what reflows
// the block, so this is where F3 actually happens.
bool ParamPanel::HandlePopUpChanged (const DG::PopUpChangeEvent& ev, bool& reflow)
{
    for (ParamControl& pc : paramControls) {
        if (ev.GetSource () != pc.control.get ())
            continue;
        reflow = ApplyVisibility ();
        return true;
    }
    return false;
}

// The three buttons a generated row can own: a FilePath's Browse, a View's
// Navigator browser, and a LibraryPart/Favourite's catalogue browser. All open a
// MODAL dialog directly on the main thread from a button handler — NEVER through
// MainThreadGate, which must not hold for human time (it would report a false
// timeout; see the gate's contract).
bool ParamPanel::HandleButtonClicked (const DG::ButtonClickEvent& ev)
{
    // evp.View — the Navigator browser. Its own loop first, because a View row's
    // button IS pc.control, not pc.browseButton.
    for (ParamControl& pc : paramControls) {
        if (pc.kind != ParamControl::Kind::NavBrowse || ev.GetSource () != pc.control.get ())
            continue;

        NavigatorBrowser browser (pc.selectedGuid);
        if (browser.Invoke () != DG::ModalDialog::Accept)
            return true; // cancelled: keep whatever the row already held

        // ⚠️ Guid and label move TOGETHER or not at all. A button showing one item's
        // text while sending another item's guid is the failure with no symptom —
        // the user reads the row they meant and the command acts on something else.
        pc.selectedGuid = browser.GetSelectedGuid ();
        pc.selectedLabel = browser.GetSelectedLabel ();
        static_cast<DG::Button*> (pc.control.get ())
            ->SetText (pc.selectedLabel.IsEmpty () ? GS::UniString ("Navigator") : pc.selectedLabel);
        // The Run gate may have just been satisfied, and a show_when may depend on
        // this value — the shell re-checks both after a handled button click.
        return true;
    }

    // evp.LibraryPart / evp.Favourite — the catalogue browser. Its button is
    // pc.control too, so it needs its own loop for the same reason View's does.
    for (ParamControl& pc : paramControls) {
        if (pc.kind != ParamControl::Kind::Catalog || ev.GetSource () != pc.control.get ())
            continue;
        OpenCatalogBrowser (pc);
        return true;
    }

    for (ParamControl& pc : paramControls) {
        if (pc.browseButton == nullptr || ev.GetSource () != pc.browseButton.get ())
            continue;

        // "Any file" is FTM::RootGroup, and it takes BOTH calls. A command may
        // instead declare arbitrary extensions (e.g. E57), which are not
        // necessarily registered in Archicad's global File Type Manager. Build
        // those filters in a short-lived private manager for this dialog.
        //
        // AddFilter only fills the filter popup. What the dialog VALIDATES the chosen
        // file against is its filter ROOT, and the default root is UnknownGroup — so
        // every file was refused ("selected file is not all files (*.*)") no matter
        // what the popup said. The DevKit's own users always pair the two:
        // ObjectStateProcessor.cpp does AddFilter (type) + SetFilterRoot (group).
        FTM::FileTypeManager fileTypeManager ("Tapioca.FilePath");
        FTM::GroupID filterRoot;
        const bool saveFile = pc.fileMode == "save";
        DG::FileDialog dialog (saveFile ? DG::FileDialog::Save : DG::FileDialog::OpenFile);
        if (pc.fileExtensions.IsEmpty ()) {
            filterRoot = FTM::RootGroup;
            dialog.SetFilterRoot (filterRoot);
            dialog.AddFilter (filterRoot);
        }
        else {
            filterRoot = fileTypeManager.AddGroup ("Tapioca files");
            for (const GS::UniString& rawExtension : pc.fileExtensions) {
                const std::string extension = NormalizedFileExtension (rawExtension);
                if (extension.empty () || extension == "*")
                    continue;

                const std::string description = "Tapioca file (*." + extension + ")";
                const FTM::FileType fileType (description.c_str (), extension.c_str (), 0, 0, 0);
                const FTM::TypeID typeID = fileTypeManager.AddType (fileType, filterRoot);
                if (typeID != FTM::UnknownType)
                    dialog.AddFilter (typeID);
            }
            dialog.SetFilterRoot (filterRoot);
        }
        if (!dialog.Invoke ()) {
            // Cancel and refusal look the same from here, so say which one this was —
            // a Browse button that silently does nothing is unreportable.
            const GS::UniString dialogMode = saveFile ? "save" : "open";
            AppendTextLine (ScanLogPath (), GS::UniString::Printf ("  FilePath '%T': the %T dialog returned nothing "
                                                                   "(cancelled, or the selection was refused).",
                                                                   pc.name.ToPrintf (), dialogMode.ToPrintf ()));
            return true;
        }

        GS::UniString path;
        const GSErrCode pathErr = dialog.GetSelectedFile ().ToPath (&path);
        if (pathErr == NoError) {
            static_cast<DG::TextEdit*> (pc.control.get ())->SetText (path);
        }
        else {
            AppendTextLine (ScanLogPath (),
                            GS::UniString::Printf ("  FilePath '%T': a file was chosen but its location "
                                                   "would not convert to a path (error %d) - the field is "
                                                   "unchanged.",
                                                   pc.name.ToPrintf (), (int) pathErr));
        }
        return true;
    }
    return false;
}

} // namespace evp
