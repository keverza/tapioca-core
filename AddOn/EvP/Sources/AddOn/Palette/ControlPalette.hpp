#ifndef GEOMETRYSERVER_CONTROLPALETTE_HPP
#define GEOMETRYSERVER_CONTROLPALETTE_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "DGModule.hpp"

// The sub-objects this shell delegates to. evp::ParamControl lives with the panel
// that builds it (Palette/ParamPanel.hpp), not here.
#include "Palette/ParamPanel.hpp"
#include "Palette/DescriptionPanel.hpp"
#include "Palette/ResultsTable.hpp"
#include "Palette/ActionBar.hpp"
#include "Palette/SelectionSetPanel.hpp"
#include "Palette/CommandListPanel.hpp"
#include "Palette/PaletteContextMenu.hpp"
#include "Palette/ServerBand.hpp"
#include "Palette/PaletteScroll.hpp"
// evp::CommandInfo and the scan that produces it — likewise not here.
#include "Palette/CommandScan.hpp"
// EVP_PRODUCT_NAME / ADDON_VERSION for the shell's user-facing status text and the
// commands.log run header (F7). Included HERE and not in ControlPalette.cpp because
// that file sits exactly on its recorded size cap in tools/quality/check_cpp.py,
// and a macro-only header is not the shell gaining a responsibility.
#include "AddOnVersion.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

// The EvP palette — server control AND the command system in ONE panel.
//
// Commands are scanned from folders, listed by category, and selecting one
// GENERATES its parameter controls below the list — see Palette/ParamPanel, which
// owns that block, and Palette/ResultsTable, which owns the results table. This
// class is the SHELL: the singleton, the DG event subscription (it is the sole
// registered observer, and the sub-objects' handlers are called from here), the
// vertical band computation in Layout(), both splitters, and placement persistence.
//
// Two things that are easy to get wrong here:
//   * a dynamically created DG item starts HIDDEN — each one must be Show()n, or
//     it exists and holds its value while being invisible;
//   * the .grc only seeds initial positions. The panel is resizable and the
//     generated controls change per command, so every position is recomputed in
//     Layout() from the panel's live width/height. Nothing may rely on the .grc
//     rect after construction.
// Dialog Manager handles Light/Dark itself; nothing here sets colour.

class ControlPalette final : public DG::Palette,
                             public DG::PanelObserver,
                             public DG::CheckItemObserver,
                             public DG::ButtonItemObserver,
                             public DG::UserControlObserver,
                             public DG::PopUpObserver,
                             public DG::TextEditBaseObserver,
                             public DG::ListBoxObserver,
                             public DG::SplitterObserver,
                             // F4 — the scrolled band's bar. The wheel arrives on
                             // DG::PanelObserver, which is already in this list.
                             public DG::ScrollBarObserver {
  public:
    static bool HasInstance ();
    static void CreateInstance ();
    static ControlPalette& GetInstance ();
    static void DestroyInstance ();

    void Show (bool focusSearch = false);
    void Hide ();

    void SetCommandStatus (const GS::UniString& text);

    // E3 — the non-seizing selection rung. A running command (on its worker
    // thread) asks the user to select and press Continue, then POLLS these flags
    // rather than seizing an ACAPI_UserInput_* pick — which was measured to block
    // the main-thread gate entirely (4/4). Begin/End touch DG and MUST be called
    // on the main thread (the dispatcher Posts them); the Is* reads are atomic and
    // safe to call from the worker thread.
    void BeginSelectionPrompt (const GS::UniString& message);
    void EndSelectionPrompt ();

    // Feature D — a one-shot results table (evp.ui.table). Runs on the main
    // thread (the dispatcher Posts it, never Invoke — see SetStatus). Each row is a
    // JSON string {"cells":[...]}; ShowResults replaces whatever was there before.
    void ShowResults (const GS::Array<GS::UniString>& headers, const GS::Array<GS::UniString>& rowJsons);
    // Plain selectable result text. Kept beside ShowResults so legacy table users
    // remain untouched while report-style commands avoid grid geometry entirely.
    void ShowResultText (const GS::UniString& text);
    // Snapshot the generated controls as the JSON shape a new run receives.
    // Called through MainThreadGate by a long-running command that supports live
    // parameter changes; touching DG directly from its worker is forbidden.
    GS::UniString CurrentParamsJson () const
    {
        return params.CollectJson ();
    }
    // E9 — a run has finished (or been cancelled). Called on the main thread from
    // the worker's completion Post: sets the final status, clears the run state and
    // hides the Stop button. `generation` is the run's own token generation, so a
    // late completion from an abandoned run cannot clear the state of a newer one.
    void FinishRun (uint64_t generation, const GS::UniString& status);

    bool IsSelectionPromptContinued () const
    {
        return promptContinued.load ();
    }
    bool IsSelectionPromptCancelled () const
    {
        return promptCancelled.load ();
    }
    bool IsSelectionPromptActive () const
    {
        return promptActive.load ();
    }

    static GSErrCode RegisterPaletteControlCallBack ();
    static GSErrCode UnregisterPaletteControlCallBack ();

    virtual ~ControlPalette ();

  private:
    ControlPalette ();
    void Initialize ();

    // Positions every item from the panel's CURRENT size. Called on construct, on
    // resize, and whenever the generated controls change.
    //
    // F4 — everything below the status line is laid out in the VIRTUAL coordinates
    // `scroll` then turns into panel positions, hiding whatever falls outside the
    // viewport. With nothing to scroll the offset is 0 and the result is identical
    // to the pre-F4 layout, item for item.
    void Layout ();

    // %LOCALAPPDATA%\EvP\palette.json. Archicad does not restore an add-on
    // palette's placement for us, and the .grc's position is a fixed 0,0 — which
    // is the top-left of monitor 1 regardless of where Archicad actually is.
    void SavePlacement () const;
    void RestorePlacement ();

    void Rescan ();

    // The command list re-filtered itself (F2) — react to what that changed. The
    // filtering, the rows and the ranking all belong to evp::CommandListPanel.
    void RefreshSearchFilter ();
    // The selected command's whole block: title, description, and (through
    // ParamPanel) its generated parameter rows.
    void RebuildCommandBlock ();
    // F3 — the same block after a show_when reflowed it: rows appeared or vanished,
    // so lay out, reveal, repaint. Shared by every event that can cause it.
    void ReflowParams ();
    // `action` empty runs the command; a name runs one of its declared output
    // actions instead, down the same worker, token and completion path.
    // `menuRegion` is set only when that name came from the right-click menu — it
    // is where the click landed, and the command reads it as `ctx.region`.
    void RunSelected (const GS::UniString& action = GS::UniString (),
                      const GS::UniString& menuRegion = GS::UniString ());

    // NOTE: the command's wrapped description is NOT here — it is part of the
    // command block, so ParamPanel builds and places it (evp::ParamPanel::Rebuild).

    // What still stands between the user and a working run: an unset required
    // parameter, an empty selection, a stopped server. Empty return == ready.
    // Drives BOTH the Run button's enabled state and the status line, so the
    // button never sits there enabled waiting to fail.
    GS::UniString WhatIsMissing () const;
    void RefreshRunGate ();

    const evp::CommandInfo* SelectedCommand () const
    {
        return commandsPanel.Selected ();
    }

    virtual void CheckItemChanged (const DG::CheckItemChangeEvent& ev) override;
    virtual void ButtonClicked (const DG::ButtonClickEvent& ev) override;
    virtual void UserControlChanged (const DG::UserControlChangeEvent& ev) override;
    // F3 — a generated popup changed: the Action, or an Enum something follows.
    // Before F3 no popup was attached to anything, so this override is new ground.
    virtual void PopUpChanged (const DG::PopUpChangeEvent& ev) override;
    // Typing into a REQUIRED text field must enable Run there and then. Waiting
    // for the idle poll made a required parameter look like it was being ignored.
    virtual void TextEditChanged (const DG::TextEditChangeEvent& ev) override;
    virtual void ListBoxSelectionChanged (const DG::ListBoxSelectionEvent& ev) override;
    // Feature E — both the command list and the results table carry a horizontal
    // splitter bar directly below them, so the user can trade height between the
    // list, the parameter block, and the table INDEPENDENTLY of the panel size.
    // The Normal DG splitter draws no tracking line of its own, and reflowing the
    // whole panel on every drag STEP jittered the bar and flickered the header. So
    // the two are split: SplitterDragged moves ONLY the bar itself to follow the
    // cursor (the visible feedback, cheap, disturbs nothing else), and the actual
    // resize + reflow + persist happens once on release in SplitterDragExited.
    virtual void SplitterDragged (const DG::SplitterDragEvent& ev) override;
    virtual void SplitterDragExited (const DG::SplitterDragEvent& ev) override;

    // F4 — the two ways the scrolled band moves. The wheel works anywhere over the
    // panel (Archicad's own tool panels scroll without the user aiming at a bar);
    // the bar is the affordance that says there is more below. Tracked AND Changed:
    // DG sends Changed when the thumb is RELEASED, and a column that only moves on
    // release is not a scroll bar.
    // Esc closes the command dropdown. A modeless palette gets no key events of its
    // own — a hot key registered on the panel is the only route DG offers (there is
    // no key observer on DG::TextEdit), and it stays local to this panel.
    virtual void PanelHotkeyPressed (const DG::PanelHotKeyEvent& ev, bool* processed) override;

    virtual void PanelWheelTracked (const DG::PanelWheelTrackEvent& ev, bool* processed) override;
    virtual void ScrollBarChanged (const DG::ScrollBarChangeEvent& ev) override;
    virtual void ScrollBarTracked (const DG::ScrollBarTrackEvent& ev) override;

    // Shared by both handlers: the clamped dialog-relative y a bar may sit at, given
    // which splitter fired (nullptr if the event is from neither).
    short ClampSplitterY (const DG::SplitterDragEvent& ev, const DG::Splitter** which) const;
    virtual void PanelResized (const DG::PanelResizeEvent& ev) override;
    virtual void PanelMoved (const DG::PanelMoveEvent& ev) override;
    // Selection and server state change outside this panel, so the gate has to be
    // re-checked on idle. Throttled — it costs an ACAPI call.
    virtual void PanelIdle (const DG::PanelIdleEvent& ev) override;
    // The right-click menu — THREE routes, because DG has three and a panel-level
    // handler alone reaches only part of the palette (measured: background, buttons
    // and static text yes; edit controls and the command list no). All three land in
    // the same ShowContextMenu; see ControlPaletteMenu.cpp for which event covers
    // what, the main-thread guard, and why the menu itself is a sub-object.
    virtual void PanelContextMenuRequested (const DG::PanelContextMenuEvent& ev, bool* needHelp,
                                            bool* processed) override;
    // Every generated parameter control and the command search field attach to THIS,
    // so one override serves all of them. Declared once and it overrides the method
    // in each ItemObserver base copy — DG does not inherit ItemObserver virtually,
    // but a single derived declaration still overrides it in every base subobject.
    virtual void ItemContextMenuRequested (const DG::ItemContextMenuEvent& ev, bool* needHelp,
                                           bool* processed) override;
    // DG::ListBox intercepts the context event (it overrides SpecContextMenuRequested)
    // and re-dispatches it HERE — which is why the command list never reached the
    // panel handler. Serves the results table's list box too.
    virtual void ListBoxContextMenuRequested (const DG::ListBoxContextMenuEvent& ev, bool* processed) override;

    // Shared by all three: the guard, the position, the display and the dispatch.
    // `clicked` is the item under the pointer (null over bare panel), which is all
    // it takes to tell the regions apart. Returns true when a menu was actually
    // shown, which is what the caller reports back to DG as `processed`.
    bool ShowContextMenu (const DG::Item* clicked);
    // Which region of the palette `clicked` belongs to, in the vocabulary
    // @tapioca.menu declares against: "panel", "params", "param:<name>",
    // "commands", "results".
    GS::UniString RegionOf (const DG::Item* clicked) const;
    virtual void PanelCloseRequested (const DG::PanelCloseRequestEvent& ev, bool* accepted) override;

    static GSErrCode PaletteControlCallBack (Int32 referenceID, API_PaletteMessageID messageID, GS::IntPtr param);

    static GS::Ref<ControlPalette> instance;

    DG::PushCheck runToggle; // pressed == running
    DG::LeftText urlText;    // the server's address, and nothing else
    // PLAT-7 — the old second server line (held snapshot, bytes) is gone: it changed
    // constantly, said nothing the user acts on, and cost a row above the commands.
    // The .grc item is still BOUND and hidden rather than deleted, because .grc ids
    // are positional and removing item 3 would renumber every id after it.
    DG::LeftText statusText;
    // Retired with the two server text rows above: the command picker needs no
    // caption. Bound and hidden rather than removed — .grc ids are positional.
    DG::LeftText commandsLabel;
    // The command band's .grc list box. Bound here, as it always was, and LENT to
    // evp::CommandListPanel — which owns everything ABOUT it (the rows, the combo,
    // the scanned commands). See its header for why it stays here.
    DG::SingleSelListBox commandList;
    DG::Button rescanButton; // also resets params to defaults
    DG::Button runButton;
    DG::LeftText commandStatus;
    // Retired (PLAT-F13): the command's name lives in the description band's fold
    // header now. Held only to keep the .grc item hidden - deleting item [9] would
    // renumber the pen pool at [10]..[17] and every id in ResourceIds.hpp with it.
    DG::LeftText commandTitleText; // .grc PaletteParamsHeaderId

    // The server and its one button. Declared AFTER runToggle, which it borrows —
    // members are destroyed in reverse declaration order.
    evp::ServerBand serverBand;

    // E3 selection prompt. The button is created dynamically (like a FilePath
    // Browse button) so no .grc item — and thus no positional-id renumber — is
    // needed; it stays hidden until BeginSelectionPrompt shows it.
    std::unique_ptr<DG::Button> continueButton;
    std::atomic<bool> promptActive { false };
    std::atomic<bool> promptContinued { false };
    std::atomic<bool> promptCancelled { false };

    // E9/F1 — the run this palette is showing. There is no separate Stop button:
    // `runButton` changes role (Run / Cancel / Stopping...), because a button whose
    // label always matches what pressing it does cannot be pressed at the wrong
    // moment. The actual cancel flag lives in evp::RunCancel, not here: the
    // dispatcher and the external runner have to read it from worker threads without
    // pulling DG/ACAPI into their translation units. `runActive` is this palette's
    // view of the same fact and drives the button's role; `stopRequested` is the
    // third state — the cancel has been asked for but the run has not ended yet, so
    // the button says so instead of inviting a second, meaningless press. Both are
    // cleared by FinishRun, which every ending (success, error, timeout cancel,
    // panel-close cancel) routes through.
    // Archicad has told us it is busy (APIPalMsg_DisableItems_Begin/End — project
    // open/close, mostly). The idle poll stands down while it is set: it reads
    // attribute pickers back, and those belong to the project being swapped.
    std::atomic<bool> itemsDisabled { false };
    bool eventProcessingStarted = false;

    std::atomic<bool> runActive { false };
    std::atomic<bool> stopRequested { false };
    uint64_t runGeneration = 0; // main thread only

    // Feature D — the one-shot results table (evp.ui.table). It owns its list box,
    // its cached content, its height and its visibility; this shell owns the band it
    // sits in, the splitter below it, and the DG event subscription.
    evp::ResultsTable results;
    // What the user can DO with what the last run produced - one button per
    // action the command DECLARED, executed by the framework from the stored
    // result. See ActionBar.hpp for why it is a band and not a menu.
    evp::ActionBar actionBar;
    evp::SelectionSetPanel selectionSets;

    // PLAT-F13 — the selected command's description, as a band that folds away.
    // It owns its header, its wrapped lines and its height; this shell owns the
    // band it sits in, the splitter below it, and the DG event subscription.
    evp::DescriptionPanel description;

    // The palette's right-click menu: its commands, its labels and its one
    // Display call. Borrows nothing and is borrowed by nothing, so its position
    // among these members carries no destruction-order meaning.
    evp::PaletteContextMenu contextMenu;
    // When the last menu CLOSED. DG can raise more than one context event for a
    // single right-click (an item's and then the panel's), and two menus in a row
    // for one click is a bug the user sees. See ShowContextMenu.
    std::chrono::steady_clock::time_point lastContextMenuAt {};

    // Feature E — the two splitter bars, and the resizable heights they drive.
    // Both bars are built at runtime like the Continue button so they need no .grc
    // item. commandListSplitter is always visible; tableSplitter shows only while
    // the results table does. Each band reports where Layout() last placed it
    // (commandsPanel.Top () / results.Top ()), so a drag can turn the splitter's y
    // back into a height.
    std::unique_ptr<DG::Splitter> commandListSplitter;
    std::unique_ptr<DG::Splitter> tableSplitter;
    // Shown only while a description is, and only while it is expanded: a bar
    // under a folded band would drag nothing.
    std::unique_ptr<DG::Splitter> descriptionSplitter;

    // F4 — the virtual scroll for everything below the status line: it owns the bar,
    // the offset and the clamp, and every scrolled item goes on the panel through
    // it. Layout hands it to each band's PlaceAt rather than the band keeping a
    // reference, so there is exactly one place the offset is applied.
    evp::PaletteScroll scroll;

    GS::UniString lastGateMessage; // so the status line is not rewritten every idle
    UInt32 idleTicks = 0;

    // What RegisterHotKey handed back for Esc — the id the hot-key event reports, so
    // it has to be kept to tell our key from anyone else's.
    short escapeHotKey = 0;

    // Native pen swatches from the .grc, bound once and reused. Hidden unless a
    // command's parameter claims one — see PalettePenPoolFirstId. ParamPanel only
    // BORROWS these (ParamControl::penSwatch is a raw pointer into this pool), so
    // the pool must outlive it: `params` is declared LAST on purpose, and members
    // are destroyed in reverse declaration order.
    std::vector<std::unique_ptr<DG::UserControl>> penPool;

    // The command band: the combo, its rows, and the scanned commands.
    // Declared AFTER commandList, which it borrows — members are
    // destroyed in reverse declaration order.
    evp::CommandListPanel commandsPanel;

    // The generated parameter block. Declared last — see penPool above.
    evp::ParamPanel params;
};

#endif
