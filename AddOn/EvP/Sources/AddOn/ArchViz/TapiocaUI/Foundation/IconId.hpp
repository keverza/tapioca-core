#ifndef EVP_ARCHVIZ_TAPIOCAUI_FOUNDATION_ICONID_HPP
#define EVP_ARCHVIZ_TAPIOCAUI_FOUNDATION_ICONID_HPP

// TapiocaUI/Foundation/IconId -- every glyph the HUD can draw, by a number that never changes.
//
// ⚠️ APPEND-ONLY, NEVER RENUMBERED. A project or a saved layout may keep an id, so a value once
// given is that glyph's for good. A glyph no longer wanted keeps its entry and is pointed at
// `None`'s placeholder by the generator; its number is never reused.
//
// ⚠️ HAND-MAINTAINED; THE GENERATOR READS IT. Each entry is `Name = number, // upstream.svg`: the
// file under AddOn/EvP/Icons/ the glyph is generated from, which is Iconoir's own name for it.
// tools/generate_iconoir_icons.py turns those files into Iconoir.gen.cpp; adding an entry here
// without regenerating fails tests/test_generate_iconoir_icons.py.
//
// Iconoir v7.12.1 (https://iconoir.com) - MIT, Copyright (c) 2021 Luca Burgio.

#include <cstdint>

namespace geomsrv {
namespace archviz {
namespace ui {

enum class IconId : uint16_t {
    None = 0, // the placeholder a missing or retired glyph draws as
    // Generic
    Search = 1,     // search.svg
    Settings = 2,   // settings.svg
    Lock = 3,       // lock.svg
    Edit = 4,       // edit-pencil.svg
    ArrowLeft = 5,  // arrow-left.svg
    ArrowRight = 6, // arrow-right.svg
    // The HUD's own tabs
    Bug = 7,           // bug.svg
    CursorPointer = 8, // cursor-pointer.svg
    // Editor tools
    AlignHorizontalCenters = 9, // align-horizontal-centers.svg
    Cut = 10,                   // cut.svg
    DesignNib = 11,             // design-nib.svg
    Copy = 12,                  // copy.svg
    BooleanExclude = 13,        // exclude.svg
    BooleanCombine = 14,        // combine.svg
    BooleanUnion = 15,          // union.svg
    BooleanSubtract = 16,       // substract.svg -- upstream's spelling
    BooleanIntersect = 17,      // intersect.svg
    FrameSimple = 18,           // frame-simple.svg
    MagicWand = 19,             // magic-wand.svg
    Droplet = 20,               // droplet.svg
    // More of the HUD's tabs (the user, 2026-10-21)
    ReportColumns = 21, // report-columns.svg: Stats
    Glasses = 22,       // glasses.svg: the viewer's Analysis
    HomeSimple = 23,    // home-simple.svg: massing feasibility
};

} // namespace ui
} // namespace archviz
} // namespace geomsrv

#endif
