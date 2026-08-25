#include "ControlPalette.hpp"
#include "Palette/PaletteMetrics.hpp"   // Margin / RowHeight / … shared with the sub-objects
#include "Palette/PalettePlacement.hpp" // palette.json — the schema, not the panel
#include "ResourceIds.hpp"
#include "Python/PathUtils.hpp" // AppendTextLine / ScanLogPath
#include "Python/RunCancel.hpp" // E9 — the running command's cancel token

#include <algorithm>

// The palette's identity. Defined here, USED here (the DG::Palette base) and in
// PaletteRegistration.cpp, which registers the modeless window on its hash — so it
// needs external linkage, and a namespace-scope `const` does not have it by default.
extern const GS::Guid paletteGuid;
const GS::Guid paletteGuid ("{6B7C1E2A-9F44-4D8C-AA10-2E3F5C7B9D01}");

GS::Ref<ControlPalette> ControlPalette::instance;

using namespace evp::palette; // Margin, RowHeight, RowGap, ButtonWidth, SplitterBarHeight

namespace {

// Standard Archicad spacing: multiples of 4/8. Margin / RowHeight / RowGap /
// ButtonWidth / SplitterBarHeight / BottomMargin / ActionButtonHeight are shared
// with the sub-objects and the band layout, and live in
// Palette/PaletteMetrics.hpp. Each band's own minimum belongs to the band:
// evp::CommandListPanel::MinHeight, evp::ResultsTable::MinHeight,
// evp::DescriptionPanel::MinHeight.

} // namespace

// ---------------------------------------------------------------------------
ControlPalette::ControlPalette ()
    : DG::Palette (ACAPI_GetOwnResModule (), GeometryServerPaletteResId, ACAPI_GetOwnResModule (), paletteGuid),
      runToggle (GetReference (), PaletteToggleButtonId), urlText (GetReference (), PaletteUrlTextId),
      statusText (GetReference (), PaletteStatusTextId), commandsLabel (GetReference (), PaletteCommandsLabelId),
      commandList (GetReference (), PaletteCommandListId), rescanButton (GetReference (), PaletteRescanButtonId),
      runButton (GetReference (), PaletteRunButtonId), commandStatus (GetReference (), PaletteCommandStatusId),
      commandTitleText (GetReference (), PaletteParamsHeaderId),
      // The shell is the sole DG observer, so every sub-object attaches the items it
      // builds to THIS, never to itself. None creates a DG item here; see the
      // Create () calls in the body. `params` borrows the pen pool, `commandsPanel`
      // the .grc list box and `serverBand` the toggle and the address line — see the
      // header.
      serverBand (runToggle, urlText), results (*this, *this), preview (*this, *this, *this, *this, *this),
      actionBar (*this, *this), selectionSets (*this, *this), description (*this, *this), scroll (*this, *this),
      commandsPanel (*this, *this, commandList), params (*this, *this, penPool)
{
    evp::StartupTrace ("ControlPalette: constructor entered");
}

void ControlPalette::Initialize ()
{
    evp::StartupTrace ("ControlPalette: initialization entered");
    Attach (*this);
    runToggle.Attach (*this);
    rescanButton.Attach (*this);
    runButton.Attach (*this);
    commandList.Attach (*this);

    statusText.Hide ();
    commandsLabel.Hide ();
    // PLAT-F13 retired: the description band's fold header names the command, and a
    // panel printing that name twice read as a bug. Hidden rather than deleted - see
    // the note beside item [9] in the .grc.
    commandTitleText.Hide ();

    // DG exposes no TextEdit key event, so Esc uses the panel hot-key route.
    escapeHotKey = RegisterHotKey (DG::Key::Escape);

    // TextEditChanged fires only on commit; opt-in idle polling provides live search.

    // Build every runtime item before event processing can expose the panel.
    commandsPanel.Create ();
    scroll.Create ();

    params.Create ();

    // Bind the .grc pen swatches once and hide them. Unlike a generated control, a
    // .grc item starts VISIBLE, so an unclaimed swatch would sit on the panel doing
    // nothing. SetUserControlCallback is what turns each into a real pen chooser;
    // without it the item draws as an empty bevelled box.
    for (short i = 0; i < PenPoolSize; ++i) {
        auto swatch = std::make_unique<DG::UserControl> (GetReference (), (short) (PalettePenPoolFirstId + i));

        API_UCCallbackType ucb = {};
        ucb.dialogID = GetId ();
        ucb.itemID = (short) (PalettePenPoolFirstId + i);
        ucb.type = APIUserControlType_Pen;
        const GSErrCode err = ACAPI_Dialog_SetUserControlCallback (&ucb);
        if (err != NoError) {
            // Drop it rather than keep a dead item: an unbound UserControl draws as
            // an empty bevelled box that does nothing when clicked. A shorter pool
            // just means pen parameters fall back to a number field, loudly.
            evp::AppendTextLine (evp::ScanLogPath (),
                                 GS::UniString::Printf ("pen pool: SetUserControlCallback failed on item %d (error %d) "
                                                        "- this slot is unusable; pen parameters beyond the remaining "
                                                        "slots fall back to a 1..255 number field.",
                                                        (int) ucb.itemID, (int) err));
            swatch->Hide ();
            continue;
        }
        swatch->Attach (*this);
        swatch->SetMin (1);
        swatch->SetMax (255);
        swatch->Hide ();
        penPool.push_back (std::move (swatch));
    }

    // Runtime creation avoids renumbering the positional .grc item ids.
    continueButton = std::make_unique<DG::Button> (*this, DG::Rect (Margin, 0, Margin + ButtonWidth, 28));
    continueButton->SetText ("Continue");
    continueButton->Attach (*this);
    continueButton->Hide ();

    results.Create ();
    preview.Create ();
    description.Create ();

    // Horizontal splitters report a dialog-relative y used as the new band height.
    commandListSplitter = std::make_unique<DG::Splitter> (*this, DG::Rect (Margin, 0, Margin + 100, SplitterBarHeight),
                                                          DG::Splitter::Horizontal, DG::Splitter::Normal);
    commandListSplitter->Attach (*this);
    commandListSplitter->EnableDrag ();

    tableSplitter = std::make_unique<DG::Splitter> (*this, DG::Rect (Margin, 0, Margin + 100, SplitterBarHeight),
                                                    DG::Splitter::Horizontal, DG::Splitter::Normal);
    tableSplitter->Attach (*this);
    tableSplitter->EnableDrag ();
    tableSplitter->Hide ();

    descriptionSplitter = std::make_unique<DG::Splitter> (*this, DG::Rect (Margin, 0, Margin + 100, SplitterBarHeight),
                                                          DG::Splitter::Horizontal, DG::Splitter::Normal);
    descriptionSplitter->Attach (*this);
    descriptionSplitter->EnableDrag ();
    descriptionSplitter->Hide ();

    // A DOCKABLE palette shows Archicad's docking preview (the blue overlay)
    // while you drag it, and only moves on release. This palette does not dock,
    // so turning docking off restores normal live dragging — which is what
    // Find & Select does, and it is still resizable.
    DisableDock (DG_DOCK_ALL);

    RestorePlacement ();
    Layout ();
    BeginEventProcessing ();
    eventProcessingStarted = true;
    EnableHotKeys ();
    EnableIdleEvent ();
    evp::StartupTrace ("ControlPalette: DG initialization complete");
    serverBand.Refresh (); // the button starts blank — it is built from the .grc
    Rescan ();
    evp::StartupTrace ("ControlPalette: initialization complete");
}

ControlPalette::~ControlPalette ()
{
    selectionSets.Clear ();
    params.Clear ();
    if (eventProcessingStarted)
        EndEventProcessing ();
}

bool ControlPalette::HasInstance ()
{
    return instance != nullptr;
}

void ControlPalette::CreateInstance ()
{
    if (!HasInstance ()) {
        evp::StartupTrace ("ControlPalette: creating instance");
        try {
            instance = new ControlPalette ();
            evp::StartupTrace ("ControlPalette: instance published");
            instance->Initialize ();
        }
        catch (...) {
            evp::StartupTrace ("ControlPalette: initialization threw; discarding instance");
            instance = nullptr;
            return;
        }
        ACAPI_KeepInMemory (true);
    }
}

ControlPalette& ControlPalette::GetInstance ()
{
    return *instance;
}
void ControlPalette::DestroyInstance ()
{
    instance = nullptr;
}
void ControlPalette::Show (bool focusSearch)
{
    DG::Palette::Show ();
    if (focusSearch)
        commandsPanel.FocusSearch ();
}
void ControlPalette::Hide ()
{
    DG::Palette::Hide ();
}

void ControlPalette::SetCommandStatus (const GS::UniString& text)
{
    commandStatus.SetText (text);
}

void ControlPalette::PanelIdle (const DG::PanelIdleEvent&)
{
    // Server state changes outside this panel, and DG's text-edit change event
    // fires on commit (Tab/Enter/focus loss) rather than per keystroke — so this
    // is the backstop that keeps Run from looking stuck after typing.
    //
    // No longer throttled: WhatIsMissing() stopped calling ACAPI when selection
    // was dropped from the gate, so it is now just string and pointer checks.
    // The throttle only bought latency.
    //
    // But it is NOT free of Archicad: the run gate reads attribute pickers back,
    // and those belong to the project. While Archicad says it is busy (project
    // open/close — see PaletteControlCallBack) this poll does nothing at all.
    // Nothing here is urgent enough to be worth asking a half-loaded project.
    if (itemsDisabled.load ())
        return;

    RefreshRunGate ();

    RefreshSearchFilter ();
    preview.PollRetained ();
}

// F2 — see the header. One string compare per idle is the whole cost.
void ControlPalette::RefreshSearchFilter ()
{
    bool selectionChanged = false;
    if (!commandsPanel.RefreshSearch (selectionChanged))
        return;

    // A table belongs to the command that produced it — a filter that moved the
    // selection must not leave its table hanging under the new one's parameters.
    if (selectionChanged)
        results.Clear ();

    RebuildCommandBlock ();
    Redraw (); // rows vacated by a shorter list keep their old pixels otherwise
}

// Feature E — the y a bar may sit at, clamped so its list can neither collapse below
// its min nor grow past what leaves the panel usable. `which` is set to whichever
// splitter fired, or nullptr if the event is from neither. Height is always (y - top),
// so the caller turns a clamped y straight into a height with the same subtraction.
short ControlPalette::ClampSplitterY (const DG::SplitterDragEvent& ev, const DG::Splitter** which) const
{
    const short maxSpan = (short) GS::Max (
        (short) (evp::CommandListPanel::MinHeight + evp::ResultsTable::MinHeight + 200), (short) (GetHeight () - 160));
    *which = nullptr;

    // The command picker is in the FIXED head, so its bar's dialog y IS its layout y —
    // no scroll offset is involved. Adding one here would make the dropdown jump by
    // the scroll distance every time the column below it was scrolled.
    if (commandListSplitter && ev.GetSource () == commandListSplitter.get ()) {
        *which = commandListSplitter.get ();
        const short listTop = commandsPanel.Top ();
        const short h = (short) GS::Max (evp::CommandListPanel::MinHeight,
                                         GS::Min ((short) (ev.GetPosition () - listTop), maxSpan));
        return (short) (listTop + h);
    }
    // F4 — the results table IS in the scrolled column, and its bar's drag reports a
    // DIALOG y while the band was laid out in VIRTUAL coordinates, so the offset has
    // to go back on.
    if (tableSplitter && ev.GetSource () == tableSplitter.get ()) {
        *which = tableSplitter.get ();
        const short pos = scroll.ToVirtual (ev.GetPosition ());
        const short tableTop = results.Top ();
        const short h = (short) GS::Max (evp::ResultsTable::MinHeight, GS::Min ((short) (pos - tableTop), maxSpan));
        return (short) (tableTop + h);
    }
    // PLAT-F13 — the description band, also in the scrolled column. Its upper
    // clamp is its OWN content, not maxSpan: there is nothing below the last
    // line to reveal, so letting it grow further would only open a gap.
    if (descriptionSplitter && ev.GetSource () == descriptionSplitter.get ()) {
        *which = descriptionSplitter.get ();
        const short pos = scroll.ToVirtual (ev.GetPosition ());
        const short textTop = description.Top ();
        const short ceiling = (short) GS::Min (description.ContentHeight (), maxSpan);
        const short h = (short) GS::Max (evp::DescriptionPanel::MinHeight, GS::Min ((short) (pos - textTop), ceiling));
        return (short) (textTop + h);
    }
    return 0;
}

// During the drag, move ONLY the bar the cursor holds — the panel does not reflow, so
// there is nothing to jitter or flicker, but the bar visibly tracks the cursor (the
// feedback the Normal splitter does not draw itself). The committed resize waits for
// release; if the user drags off the top/bottom the clamp pins the bar at its limit.
void ControlPalette::SplitterDragged (const DG::SplitterDragEvent& ev)
{
    const DG::Splitter* which = nullptr;
    const short y = ClampSplitterY (ev, &which);
    if (which == nullptr)
        return;
    const short right = (short) (GetWidth () - Margin);
    const DG::Rect bar (Margin, y, right, (short) (y + SplitterBarHeight));
    // The command bar is in the fixed head and goes straight on the panel; the table's
    // is in the scrolled column, where `y` is virtual and the bar must not be drawn
    // over the fixed head above it.
    if (which == commandListSplitter.get ())
        const_cast<DG::Splitter*> (which)->SetRect (bar);
    else
        scroll.Place (const_cast<DG::Splitter*> (which), bar);
}

// On release, turn the bar's final y back into a height, reflow the whole panel to it
// once, and persist. One clean layout, no mid-drag churn.
void ControlPalette::SplitterDragExited (const DG::SplitterDragEvent& ev)
{
    const DG::Splitter* which = nullptr;
    const short y = ClampSplitterY (ev, &which);
    if (which == commandListSplitter.get () && which != nullptr)
        commandsPanel.SetHeight ((short) (y - commandsPanel.Top ()));
    else if (which == tableSplitter.get () && which != nullptr)
        results.SetHeight ((short) (y - results.Top ()));
    else if (which == descriptionSplitter.get () && which != nullptr)
        description.SetHeight ((short) (y - description.Top ()));
    else
        return;

    Layout ();
    SavePlacement ();
}

// Esc collapses the command dropdown, and means nothing otherwise — `processed` is
// set only when there was something open to close, so Esc keeps whatever meaning
// Archicad gives it the rest of the time.
void ControlPalette::PanelHotkeyPressed (const DG::PanelHotKeyEvent& ev, bool* processed)
{
    if (ev.GetKeyId () != escapeHotKey || !commandsPanel.IsOpen ())
        return;

    commandsPanel.CloseList ();
    Layout ();
    Redraw ();
    if (processed != nullptr)
        *processed = true;
}

// The bar was released on a new value.
void ControlPalette::ScrollBarChanged (const DG::ScrollBarChangeEvent& ev)
{
    if (!preview.HandleScrollBarChanged (ev) && scroll.IsSource (ev.GetSource ()) && scroll.FollowBar ())
        Layout ();
}

// ...and while the thumb is still held. Both are needed: a column that only moved
// when the thumb was let go would not read as a scroll bar at all.
void ControlPalette::ScrollBarTracked (const DG::ScrollBarTrackEvent& ev)
{
    if (!preview.HandleScrollBarTracked (ev) && scroll.IsSource (ev.GetSource ()) && scroll.FollowBar ())
        Layout ();
}

void ControlPalette::PanelResized (const DG::PanelResizeEvent& /*ev*/)
{
    Layout ();
    SavePlacement ();
}

void ControlPalette::PanelMoved (const DG::PanelMoveEvent& /*ev*/)
{
    SavePlacement ();
}

// The file itself — schema, validation, IO — is Palette/PalettePlacement. These
// two only translate between it and the live panel.
void ControlPalette::SavePlacement () const
{
    const DG::NativePoint position = GetClientPosition ();
    evp::PalettePlacement p;
    p.left = (short) position.GetX ().GetValue ();
    p.top = (short) position.GetY ().GetValue ();
    p.width = GetWidth ();
    p.height = GetHeight ();
    p.listHeight = commandsPanel.Height ();
    p.resultsHeight = results.Height ();
    p.descriptionHeight = description.Height ();
    p.descriptionCollapsed = description.IsCollapsed ();
    evp::SavePalettePlacement (p);
}

void ControlPalette::RestorePlacement ()
{
    // 0 means the file had nothing usable for that field, so each default survives.
    const evp::PalettePlacement p = evp::LoadPalettePlacement (
        evp::CommandListPanel::MinHeight, evp::ResultsTable::MinHeight, evp::DescriptionPanel::MinHeight);

    if (p.width > 0)
        SetClientSize (p.width, p.height);
    if (p.listHeight > 0)
        commandsPanel.SetHeight (p.listHeight);
    if (p.resultsHeight > 0)
        results.SetHeight (p.resultsHeight);
    if (p.descriptionHeight > 0)
        description.SetHeight (p.descriptionHeight);
    // No `> 0` guard: false is a real saved value, not "unset".
    description.SetCollapsed (p.descriptionCollapsed);
    if (p.hasPosition)
        SetClientPosition (DG::NativeUnit (p.left), DG::NativeUnit (p.top));
}

// The scan and the rows it produces belong to the command-list band; this only
// clears what the previous command left behind and shows the band's answer.
void ControlPalette::Rescan ()
{
    params.Clear ();
    selectionSets.Clear ();
    // A table belongs to the command that produced it, and a rescan can change
    // which command is selected — so its table must not outlive it.
    results.Clear ();

    const GS::UniString status = commandsPanel.Rescan ();
    SetCommandStatus (status);
    lastGateMessage = status; // or the idle poll stamps over the count

    RebuildCommandBlock ();
}

// The selected command's whole block: its description band (whose fold header
// carries the command's name) and the parameter rows built from the scan.
void ControlPalette::RebuildCommandBlock ()
{
    params.Clear ();
    selectionSets.Clear ();
    description.Clear ();
    actionBar.Clear ();

    const evp::CommandInfo* const info = SelectedCommand ();
    preview.SetKind (info != nullptr ? info->previewKind : GS::UniString ("text"));
    if (info == nullptr) {
        // Still lay out: the rows just cleared have to be given up, and a query that
        // matches nothing is reached WITH the dropdown open — its "no command
        // matches" row is the whole point of that state and needs placing.
        Layout ();
        RefreshRunGate ();
        return;
    }
    // The description band (PLAT-F13) names the command in its own fold header, so
    // there is no separate title line above it. The folded state is NOT reset here:
    // a user who folded the description away meant it for the palette, not for one
    // command - re-opening it per selection would undo that faster than they chose.
    description.Rebuild (info->title, info->description);
    // Built now, dead until a run completes: an action acts on the result the
    // last run stored, and there is nothing stored before that.
    actionBar.Rebuild (*info);

    params.Rebuild (*info);
    selectionSets.Rebuild (info->selectionSets);

    // Position everything BEFORE revealing it: a dynamically created DG item starts
    // hidden at its construction rect, and showing it first would make every control
    // flash in the panel's top-left corner on the way to its real place.
    Layout ();
    params.ShowControls ();

    // A freshly built set of controls changes what is missing — recheck before the
    // user can press anything.
    RefreshRunGate ();
}

// ---------------------------------------------------------------------------
// Feature D — the one-shot results table (evp.ui.table). The table itself lives in
// Palette/ResultsTable; this shell only owns its band, its splitter and the DG
// events. ShowResults keeps its name and signature: ApiDispatcher calls it.
void ControlPalette::ShowResults (const GS::Array<GS::UniString>& headers, const GS::Array<GS::UniString>& rowJsons)
{
    results.Show (headers, rowJsons);

    Layout (); // reserves the table's row, picks its scroll type, sizes its columns
    // Posted from the dispatcher, OUTSIDE the DG event flow that would repaint — so
    // force a redraw, exactly as BeginSelectionPrompt does for the same reason.
    Redraw ();
}

void ControlPalette::ShowResultText (const GS::UniString& text)
{
    results.ShowText (text);
    Layout ();
    Redraw ();
}

void ControlPalette::CheckItemChanged (const DG::CheckItemChangeEvent& ev)
{
    // A generated row's control (a checkbox, or an attribute picker's PushCheck
    // host) — the panel that built it handles it and says so.
    bool reflow = false;
    if (params.HandleCheckItemChanged (ev, reflow)) {
        if (reflow)
            ReflowParams ();
        RefreshRunGate (); // a required layer may have just been set
        return;
    }

    // The server toggle — the band owns what starting and stopping one means.
    if (serverBand.HandleCheckItemChanged (ev))
        RefreshRunGate (); // starting the server can unblock Run immediately
}

void ControlPalette::ButtonClicked (const DG::ButtonClickEvent& ev)
{
    if (preview.HandleButtonClicked (ev)) {
        Layout ();
        Redraw ();
        return;
    }

    // PLAT-F13 — the description's fold header. Only the band's own height
    // changes, so this is a reflow, not a rebuild; the redraw is for the rows
    // the folded text vacates, which keep their old pixels otherwise.
    if (description.IsSource (ev.GetSource ())) {
        description.SetCollapsed (!description.IsCollapsed ());
        Layout ();
        SavePlacement ();
        Redraw ();
        return;
    }

    if (ev.GetSource () == &rescanButton) {
        Rescan ();
        return;
    }
    if (ev.GetSource () == &runButton) {
        // F1 — one button, two roles, and the role IS the double-run guard: while a
        // run is in flight this branch never reaches RunSelected.
        if (runActive.load ()) {
            if (stopRequested.exchange (true))
                return; // already asked — the label already says so
            // E9 — trip the token and say so immediately. The worker cannot stop
            // instantly: it stops at its next checkpoint, its next bus call, or (Zone
            // C) when the drain loop kills the subprocess. Saying "stopping" rather
            // than "stopped" is the difference between a UI that lies and one that
            // does not.
            evp::RunCancel::Get ().Request (evp::CancelReason::StopButton);
            SetCommandStatus ("Stopping — the command ends at its next checkpoint...");
            // A prompt-waiting command is blocked in its poll loop and would never
            // reach a checkpoint, so this has to release that wait too.
            if (promptActive.load ())
                promptCancelled.store (true);
            RefreshRunGate (); // -> "Stopping...", disabled until FinishRun
            return;
        }
        RunSelected ();
        return;
    }
    if (continueButton && ev.GetSource () == continueButton.get ()) {
        // Signal the waiting command; it reads GetSelection and ends the prompt.
        // Hide the button immediately so it cannot be pressed twice.
        promptContinued.store (true);
        continueButton->Hide ();
        return;
    }

    if (selectionSets.HandleButtonClicked (ev)) {
        Layout ();
        Redraw ();
        return;
    }

    // An output action. It goes down the SAME launch path as Run - one worker,
    // one cancel token, one place an outcome lands - carrying the action's name
    // instead of an empty one. What it must NOT do is re-run the command: the
    // Python side reads the last run's stored result, so every write that run
    // performed stays performed once.
    const GS::UniString action = actionBar.ActionOf (ev.GetSource ());
    if (!action.IsEmpty ()) {
        RunSelected (action);
        return;
    }

    // A generated row's Browse button (evp.FilePath). Refreshing the gate on any
    // such press, rather than only when a file was chosen, costs nothing — the same
    // check runs on idle — and a required FilePath may have just been satisfied.
    if (params.HandleButtonClicked (ev))
        RefreshRunGate ();
}

void ControlPalette::TextEditChanged (const DG::TextEditChangeEvent& /*ev*/)
{
    // A required text parameter is empty until the user types something, so the
    // gate has to react to typing. Relying on the idle poll made a required field
    // look broken: you could type a name and Run stayed disabled.
    RefreshRunGate ();
    RefreshSearchFilter (); // the search box commits through here too
}

void ControlPalette::UserControlChanged (const DG::UserControlChangeEvent& /*ev*/)
{
    // Pen swatches only. Nothing to do: ACAPI_Dialog_SetUserControlCallback runs the
    // pen chooser and stores the new pen on the item itself, so the value is already
    // correct and ParamPanel::CollectJson reads it at Run time. This override exists
    // because DG requires the observer to be attached for the control to be live.
}

void ControlPalette::PopUpChanged (const DG::PopUpChangeEvent& ev)
{
    // The command's Action, or an Enum other rows follow. Only the parameter block
    // owns popups, so there is nothing else this can be.
    bool reflow = false;
    if (!preview.HandlePopUpChanged (ev) && params.HandlePopUpChanged (ev, reflow)) {
        if (reflow)
            ReflowParams ();
        RefreshRunGate (); // a mode change can hide the row that was blocking Run
    }
}

void ControlPalette::ListBoxSelectionChanged (const DG::ListBoxSelectionEvent& ev)
{
    if (commandsPanel.IsSource (ev.GetSource ())) {
        // A picked row IS the answer the dropdown was open for — collapse it back to
        // the selection before rebuilding, so the block below appears where the list
        // was rather than below it.
        commandsPanel.CloseList ();
        // A table belongs to the command that produced it — selecting another
        // command must not leave its table hanging under the new one's parameters.
        results.Clear ();
        RebuildCommandBlock (); // ends in Layout ()
        Redraw ();              // the collapsed list's rows would keep their pixels
        return;
    }
    // A click on a results row selects that row's element in the model. Rebuilding
    // the table clears its selection to 0, which reads there as no row — a no-op, so
    // repopulating never spuriously re-selects.
    if (results.IsSource (ev.GetSource ()))
        results.SelectRowElement (results.SelectedRow ());
}

void ControlPalette::PanelCloseRequested (const DG::PanelCloseRequestEvent&, bool* accepted)
{
    // Closing the palette while a command waits on a selection must not strand the
    // worker in its poll loop — treat it as a cancel.
    if (promptActive.load ())
        promptCancelled.store (true);
    // E9 — and closing it while a command RUNS stops the command. Before this, the
    // panel could be closed while a runaway loop kept the session busy with nothing
    // left on screen to stop it. Request is a no-op when nothing is running, so
    // closing an idle palette costs nothing.
    evp::RunCancel::Get ().Request (evp::CancelReason::PanelClosed);
    selectionSets.Clear ();
    if (runActive.load ())
        stopRequested.store (true); // F1 — reopening mid-run must not offer a Cancel
                                    // that has already been asked for
    Hide ();
    *accepted = true;
}
