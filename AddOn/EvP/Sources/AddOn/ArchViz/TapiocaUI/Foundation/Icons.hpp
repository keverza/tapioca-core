#ifndef EVP_ARCHVIZ_TAPIOCAUI_FOUNDATION_ICONS_HPP
#define EVP_ARCHVIZ_TAPIOCAUI_FOUNDATION_ICONS_HPP

// TapiocaUI/Foundation/Icons -- a glyph drawn by its IconId, and the buttons that carry one.
//
// ⚠️ CALLERS ASK BY ID, NEVER BY PATH. The glyphs are compiled in (Iconoir.gen.cpp, generated
// from the pinned Iconoir drop) and drawn as strokes into the current ImDrawList, so they tint
// like text, scale with the HUD and need no texture on either surface. Whether a glyph is later
// drawn from an atlas instead is this file's business and no caller's
// (HANDOFF-TapiocaUI-Library.md section 7).
//
// ⚠️ A TIP IS THE CALLER'S. The HUD says every hover text through hudshell::Tip, which this
// library may not include; say it straight after the button, where it reads the button as the
// last item: `if (IconButton ("cut", IconId::Cut)) ...; hudshell::Tip ("Split");`.
//
// Pure ImGui on the thread that holds the context; nothing is allocated or kept per frame.

#include "ArchViz/TapiocaUI/Foundation/IconId.hpp"

#include <imgui.h>

#include <cstdint>

namespace geomsrv {
namespace archviz {
namespace ui {

// `icon` in a `size` x `size` square whose top-left is `position`, in `colour`, into the current
// window's draw list. Strokes are Iconoir's 1.5 on 24 scaled to `size`, never thinner than a pixel.
void Icon (IconId icon, ImVec2 position, float size, ImU32 colour);

// The same, centred on `centre`, into `draw`: for a canvas that owns its own list.
void DrawIcon (ImDrawList* draw, IconId icon, ImVec2 centre, float size, ImU32 colour);

// ⚠️ A GLYPH AT REST IS GREY; ONLY THE SELECTED ONE IS FULL INK (the user, 2026-10-21: "Icons in
// light mode should be dark gray. Black should be for selected icon only"). Both are the card's
// own text colour -- dark grey and black on the light card, light grey and white on the dark one --
// so a glyph follows the theme and BeginDisabled dims it like text. Never the accent.
constexpr float kIconRestAlpha = 0.72f;
ImU32 IconInk (bool selected);

// A square button the frame's height with `icon` at two thirds of it (the toolbar's 24 px item,
// 16 px glyph). `active` shows it toggled on: the full-ink glyph on the pressed grey. True on the
// frame it is pressed; never while disabled.
bool IconButton (const char* id, IconId icon, bool active = false);

// The same with `label` after the glyph.
bool IconTextButton (const char* id, IconId icon, const char* label, bool active = false);

// For the gallery's sheet: how many ids the table holds (0 to the count less one), and each one's
// IconId.hpp name and upstream file ("" for the placeholder).
uint32_t IconCount ();
const char* IconName (IconId icon);
const char* IconFile (IconId icon);

} // namespace ui
} // namespace archviz
} // namespace geomsrv

#endif
