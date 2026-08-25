#ifndef GEOMETRYSERVER_RESOURCEIDS_HPP
#define GEOMETRYSERVER_RESOURCEIDS_HPP

// Resource ids — must match the 'STR#' ids in AddOnResources/RINT/AddOn.grc.
constexpr short InfoStringsResId = 32000; // [1] name, [2] description
constexpr short MenuStringsResId = 32500; // menu title + items

// Menu command routing — ONE MENU RESOURCE PER ITEM, each 'STR#' being the shared
// title "Tapioca" plus exactly one item, so every itemIndex here is 1. Archicad
// merges same-titled MenuCode_UserDef menus into a single main menu.
//
// ⚠️ Do not collapse these into one resource with itemIndex 1..N. A MenuCode_UserDef
// resource's leading unnumbered strings decide whether the second string is an ITEM
// or a SUBMENU TITLE (ACAPinc.h, ACAPI_MenuItem_RegisterMenu remarks), so a two-item
// block reinterprets itself and the second command lands on itemIndex 1. That was
// tried; see the comment block in AddOnResources/RINT/AddOn.grc.
//
// Every id added here needs BOTH an ACAPI_MenuItem_RegisterMenu (RegisterInterface)
// and an ACAPI_MenuItem_InstallMenuHandler (Initialize) in AddOnMain.cpp — one
// without the other is a menu that shows and does nothing, or does nothing at all.
constexpr short GeometryServerMenuResId = 32500; // "Tapioca Panel"
constexpr short GeometryServerMenuItemIndex = 1;
constexpr short AboutMenuResId = 32501; // "About Tapioca..."
constexpr short AboutMenuItemIndex = 1;
constexpr short ArchVizMenuResId = 32503; // "Tapioca 3D Viewer"
constexpr short ArchVizMenuItemIndex = 1;
constexpr short NotebookMenuResId = 32504; // "Tapioca Notebook"
constexpr short NotebookMenuItemIndex = 1;
constexpr short WebUIMenuResId = 32505; // "Tapioca WebUI panel"
constexpr short WebUIMenuItemIndex = 1;
// "Rhino.Inside" — starts the one process-wide RhinoCore + stock Grasshopper
// (PLAT-RHINO-INSIDE slice 0). One menu item for now: the Grasshopper Editor
// and Player commands the handoff describes are P1/P2 and arrive with the UI
// they need, rather than as two menu items that do the same thing today.
constexpr short GrasshopperMenuResId = 32506;
constexpr short GrasshopperMenuItemIndex = 1;

// The About box ('GDLG' 32520) — its own dialog block, so these ids are independent
// of the palette's positional ids below.
constexpr short AboutDialogResId = 32520;
constexpr short AboutOkButtonId = 1;
constexpr short AboutLogoId = 2; // 'GICN' ProductLogoIconId, placed by the .grc
constexpr short AboutVersionTextId = 3;
constexpr short AboutContactTextId = 4;
constexpr short AboutLogsButtonId = 5;
constexpr short AboutCommandsButtonId = 6;
constexpr short AboutUpdatesButtonId = 7;

// The Navigator browser ('GDLG' 32530) — what an evp.View parameter opens.
// Same positional-id warning as everywhere else: these are the .grc item ORDER.
// Neither the search field nor the map tabs have an id: both are runtime-created.
// The search field must be a DG::SearchEdit (live filtering needs a per-keystroke
// event a .grc TextEdit does not give); the tab strip would otherwise drag in one
// TabPage 'GDLG' resource per tab. See the .grc comment.
constexpr short NavigatorBrowserResId = 32530;
constexpr short NavBrowserSearchLabelId = 1;
constexpr short NavBrowserTreeId = 2;
constexpr short NavBrowserStatusTextId = 3;
constexpr short NavBrowserCancelButtonId = 4;
constexpr short NavBrowserOkButtonId = 5;

// The catalogue browser ('GDLG' 32531) — what an evp.LibraryPart or an
// evp.Favourite parameter opens. ONE resource for both: the two pickers ask the
// same question of two catalogues, and the dialog's title is set at runtime.
// Same positional-id warning; the search field is runtime-created for the same
// per-keystroke reason as the Navigator browser's.
// Three panes: search, folder tree, folder contents, with a runtime-created
// vertical DG::Splitter between the last two (its orientation is a constructor
// argument, and LayoutPanes owns every rect from the live width).
constexpr short CatalogBrowserResId = 32531;
constexpr short CatalogBrowserSearchLabelId = 1;
constexpr short CatalogBrowserTreeId = 2;
constexpr short CatalogBrowserContentsId = 3;
constexpr short CatalogBrowserStatusTextId = 4;
constexpr short CatalogBrowserCancelButtonId = 5;
constexpr short CatalogBrowserOkButtonId = 6;

// Product mark, in RFIX (non-localized) — see AddOnResources/RFIX/AddOnFix.grc.
constexpr short ProductLogoIconId = 32502;

// Iconoir-derived semantic controls. These are stable resource IDs, never a
// command-provided path: a command selects a semantic UI feature (FilePath,
// selection set), and the native palette chooses its own trusted artwork.
constexpr short PaletteIconFolderId = 32600;
constexpr short PaletteIconPlusId = 32601;
constexpr short PaletteIconMinusSquareId = 32602;
constexpr short PaletteIconRefreshId = 32603;
constexpr short PaletteIconSelectFace3dId = 32604;
constexpr short PaletteIconClearId = 32605;

// The Tapioca palette ('GDLG' 32510).
// ⚠️ These ids are POSITIONAL: ResConv numbers .grc items by their ORDER, and the
// /* [n] */ comments there are only comments. Insert an item and everything after
// it renumbers — these constants must be updated in lockstep, or a control gets
// built on the wrong item (a LeftText on a Button = clickable status text).
// Positions are recomputed at runtime by ControlPalette::Layout().
// (Menu item text is "Tapioca Panel"; the constant name predates the F7 rename and
// stays, like every other internal `EvP`/`GeometryServer` identifier.)
constexpr short GeometryServerPaletteResId = 32510;
constexpr short PaletteToggleButtonId = 1; // PushCheck: pressed == running
constexpr short PaletteUrlTextId = 2;
constexpr short PaletteStatusTextId = 3;
constexpr short PaletteCommandsLabelId = 4;
constexpr short PaletteCommandListId = 5;
constexpr short PaletteRescanButtonId = 6;
constexpr short PaletteRunButtonId = 7;
constexpr short PaletteCommandStatusId = 8;
constexpr short PaletteParamsHeaderId = 9;

// Native pen swatches, ids 10..17 — a POOL, declared up front rather than created
// per command. Every other control here is built at runtime from a (Panel&, Rect&)
// constructor, but a pen swatch cannot be: it is a DG UserControl whose SUBTYPE
// (0x0001 = pen) is an extra .grc data word, and DG::UserControl's runtime ctor
// stops at procId with no way to pass it. ACAPI_Dialog_SetUserControlCallback then
// binds an EXISTING dialog item to APIUserControlType_Pen. Both halves need an item
// that came from the .grc — hence a fixed pool, hidden until a command claims one.
// A command wanting more than PenPoolSize pens falls back to a 1..255 number field.
constexpr short PalettePenPoolFirstId = 10;
constexpr short PenPoolSize = 8;

// The ArchViz 3D viewer palette ('GDLG' 32540) — Diligent renders into it.
// ⚠️ The VIEWPORT has no id here on purpose: it is a DG::UserItem created at
// runtime (see the .grc comment and ArchViz/ArchVizPanel.hpp), so this block
// holds only the status line and can never renumber anything.
constexpr short ArchVizPaletteResId = 32540;
constexpr short ArchVizStatusTextId = 1;

// The notebook experiment palette ('GDLG' 32550). Its single Browser item fills
// the client area and resizes with it.
constexpr short NotebookPaletteResId = 32550;
constexpr short NotebookBrowserId = 1;
constexpr short NotebookHtmlResId = 32551; // 'DATA', generated NotebookUI/dist/index.html

// The WebUI command palette preview ('GDLG' 32560). Its single Browser item fills
// the client area and resizes with it.
constexpr short WebUIPaletteResId = 32560;
constexpr short WebUIBrowserId = 1;
constexpr short WebUIHtmlResId = 32561; // 'DATA', embedded WebUI/index.html

#endif
