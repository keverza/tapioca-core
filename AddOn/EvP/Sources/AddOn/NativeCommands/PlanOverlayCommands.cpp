#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/PlanOverlayCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"

#include "PlanOverlay/OverlayWindow.hpp"
#include "Diagnostics/ApiError.hpp"

#include <cwchar>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

namespace geomsrv {

namespace {

namespace po = planoverlay;

// The overlay WINDOW now lives in PlanOverlay/OverlayWindow: it is shared with
// the tracking domain (§15) and, per §19.1, it is the thing that ships. What
// stays in this file is Phase 0's question — WHERE such a window can live, and
// whether Archicad keeps working underneath it.
constexpr const wchar_t* OVERLAY_CLASS = L"EvP_PlanOverlayProbe_Window";

// A style bit we temporarily set on one of Archicad's own windows, so
// ProbeOverlayHide can put it back exactly as it was. Only ever one at a time.
static HWND s_styledWindow = nullptr;
static LONG s_styledOriginal = 0;

struct WindowInfo {
    HWND         hwnd;
    HWND         parent;
    std::wstring className;
    std::wstring text;
    int          ctrlId;
    RECT         rect;
    LONG         style;
    LONG         exStyle;
    bool         visible;
};

WindowInfo Describe(HWND hwnd)
{
    WindowInfo info = {};
    info.hwnd   = hwnd;
    info.parent = GetParent(hwnd);

    wchar_t cls[256] = {};
    GetClassNameW(hwnd, cls, 255);
    info.className = cls;

    // Document windows carry their view name here; palettes carry their title.
    // It is often the fastest way for a human to recognise the floor plan.
    wchar_t txt[256] = {};
    GetWindowTextW(hwnd, txt, 255);
    info.text = txt;

    info.ctrlId = GetDlgCtrlID(hwnd);

    GetWindowRect(hwnd, &info.rect);

    info.style   = static_cast<LONG>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    info.exStyle = static_cast<LONG>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    info.visible = IsWindowVisible(hwnd) != 0;
    return info;
}

GS::ObjectState RectState(const RECT& r)
{
    GS::ObjectState os;
    os.Add("left",   static_cast<GS::Int32>(r.left));
    os.Add("top",    static_cast<GS::Int32>(r.top));
    os.Add("right",  static_cast<GS::Int32>(r.right));
    os.Add("bottom", static_cast<GS::Int32>(r.bottom));
    return os;
}

GS::ObjectState WindowState(const WindowInfo& info)
{
    GS::ObjectState item;
    item.Add("hwnd",    static_cast<GS::Int64>(reinterpret_cast<INT_PTR>(info.hwnd)));
    item.Add("parent",  static_cast<GS::Int64>(reinterpret_cast<INT_PTR>(info.parent)));
    item.Add("class",   GS::UniString(info.className.c_str()));
    item.Add("text",    GS::UniString(info.text.c_str()));
    item.Add("ctrlId",  static_cast<GS::Int32>(info.ctrlId));
    item.Add("rect",    RectState(info.rect));
    item.Add("style",   GS::UniString::Printf("0x%08lX", static_cast<unsigned long>(info.style)));
    item.Add("exStyle", GS::UniString::Printf("0x%08lX", static_cast<unsigned long>(info.exStyle)));
    item.Add("visible", info.visible);
    return item;
}

GS::ObjectState WindowStateOf(HWND hwnd)
{
    return WindowState(Describe(hwnd));
}

BOOL CALLBACK EnumChildProc(HWND hwnd, LPARAM lParam)
{
    auto* list = reinterpret_cast<std::vector<WindowInfo>*>(lParam);
    list->push_back(Describe(hwnd));
    return TRUE;
}

bool IsOurOverlay(HWND hwnd)
{
    wchar_t cls[256] = {};
    GetClassNameW(hwnd, cls, 255);
    return wcscmp(cls, OVERLAY_CLASS) == 0;
}

// What ChildWindowFromPointEx would return, minus our own overlay.
//
// There is no "exclude this class" flag, so the sibling z-order is walked by
// hand: GW_CHILD is the TOPMOST child and GW_HWNDNEXT descends, which is the
// same order the API uses. Re-implementing it is the only way to get a chain
// that describes ARCHICAD's tree while our overlay is up.
HWND ChildAtSkippingOurs(HWND parent, POINT screenPt, bool skipTransparent)
{
    for (HWND child = GetWindow(parent, GW_CHILD);
         child != nullptr;
         child = GetWindow(child, GW_HWNDNEXT)) {
        if (!IsWindowVisible(child) || !IsWindowEnabled(child))
            continue;
        if (skipTransparent
            && (GetWindowLongPtrW(child, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0)
            continue;
        if (IsOurOverlay(child))
            continue;
        RECT r = {};
        GetWindowRect(child, &r);
        if (PtInRect(&r, screenPt))
            return child;
    }
    return nullptr;
}

// Descend from `root` towards the deepest child containing `screenPt`.
//
// ChildWindowFromPointEx takes PARENT-CLIENT coordinates, so the point is
// re-mapped at every level. `skipTransparent` is the whole point of running this
// twice: the strict chain is what EXISTS at that pixel, the hit chain is what
// the MOUSE sees. If our overlay appears in one and not the other, click-through
// is proven mechanically instead of by eye.
//
// `skipOurs` produces a THIRD chain, and it is the one to resolve the canvas
// from. Without it the probe walked itself: once the overlay exists it is the
// deepest window at that pixel and the chain STOPS there, so the caller's
// "deepest window that is not ours" is the overlay's PARENT — one level too
// shallow. Every repeated show then attached a level higher than the last, which
// is exactly what happened live: run 1 covered the plan, run 2 landed on the
// document window, run 3 on the MDI client.
std::vector<HWND> DescendChain(HWND root, POINT screenPt, bool skipTransparent,
                               bool skipOurs = false)
{
    std::vector<HWND> chain;
    if (!root || !IsWindow(root))
        return chain;

    UINT flags = CWP_SKIPINVISIBLE | CWP_SKIPDISABLED;
    if (skipTransparent)
        flags |= CWP_SKIPTRANSPARENT;

    HWND cur = root;
    chain.push_back(cur);

    for (int guard = 0; guard < 64; ++guard) {
        HWND child = nullptr;
        if (skipOurs) {
            child = ChildAtSkippingOurs(cur, screenPt, skipTransparent);
        } else {
            POINT pt = screenPt;
            ScreenToClient(cur, &pt);
            child = ChildWindowFromPointEx(cur, pt, flags);
        }
        if (child == nullptr || child == cur)
            break;
        chain.push_back(child);
        cur = child;
    }
    return chain;
}

GS::Array<GS::ObjectState> ChainState(const std::vector<HWND>& chain)
{
    GS::Array<GS::ObjectState> arr;
    for (HWND h : chain)
        arr.Push(WindowStateOf(h));
    return arr;
}

void RestoreStyledWindow()
{
    if (s_styledWindow != nullptr && IsWindow(s_styledWindow)) {
        SetWindowLongPtrW(s_styledWindow, GWL_STYLE, s_styledOriginal);
        SetWindowPos(s_styledWindow, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
                     | SWP_FRAMECHANGED);
    }
    s_styledWindow   = nullptr;
    s_styledOriginal = 0;
}

HWND ResolveHwndParam(const GS::ObjectState& params, const char* key)
{
    GS::Int64 raw = 0;
    if (!params.Get(key, raw) || raw == 0)
        return nullptr;
    HWND h = reinterpret_cast<HWND>(static_cast<INT_PTR>(raw));
    return IsWindow(h) ? h : nullptr;
}

// ---------------------------------------------------------------------------
// EvP.EnumChildWindows {} -> { ok, mainWindow, windows[], count }
//
// Every entry now carries hwnd + parent, so the caller can rebuild the TREE.
// The flat list was unreadable at 881 entries and the first run's canvas was
// missed because of it.
// ---------------------------------------------------------------------------
class EnumChildWindowsCommand : public MainThreadCommand {
public:
    GS::String GetName() const override { return "EnumChildWindows"; }

    NativeCommandResult ExecuteNative(const GS::ObjectState&, GS::ProcessControl&) const override
    {
        HWND main = ACAPI_GetMainWindow();
        if (!main || !IsWindow(main))
            return NativeCommandResult::Failure("ACAPI_GetMainWindow returned null or invalid HWND");

        std::vector<WindowInfo> children;
        EnumChildWindows(main, EnumChildProc, reinterpret_cast<LPARAM>(&children));

        GS::Array<GS::ObjectState> arr;
        for (const auto& info : children)
            arr.Push(WindowState(info));

        GS::ObjectState os;
        os.Add("mainWindow", WindowStateOf(main));
        os.Add("windows", arr);
        os.Add("count", static_cast<GS::Int32>(arr.GetSize()));
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.ProbeWindowAt {x?, y?, target?}
//   -> { ok, point, target, windowType, targetMatched, strictChain[],
//        hitChain[], hostChain[], realChild }
//
// The authoritative answer to "is the plan canvas a real HWND": descend the
// window tree at a point inside the plan. Defaults to the centre of the main
// window's client area, which lands in the document window on any normal layout.
//
// ⚠️ `target` EXTENDS THIS COMMAND RATHER THAN FORKING IT (plan §24.1). Part III
// needs the same descent against the 3D window, and a second copy of this
// command for "model3d" is the failure mode that rule exists to prevent — every
// fix would then have to be made twice. Default is today's behaviour exactly, so
// `PlanOverlayMatrix` and every existing call site are unaffected.
//
// ⚠️ IT CANNOT MOVE A WINDOW TO THE FRONT, AND MUST NOT TRY. The centre of the
// main client area lands in whichever document window is frontmost, so `target`
// is a CHECK, not an action: it reports which window actually answered and
// whether that is the one asked for. Bringing a window forward is the user's
// gesture — doing it here would silently change what the rest of a sweep is
// measuring.
//
//   "any"      (default) whatever is in front; today's behaviour
//   "plan"     require the current window to be a floor plan
//   "model3d"  require it to be the 3D model window
// ---------------------------------------------------------------------------
class ProbeWindowAtCommand : public MainThreadCommand {
public:
    GS::String GetName() const override { return "ProbeWindowAt"; }

    NativeCommandResult ExecuteNative(const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        HWND main = ACAPI_GetMainWindow();
        if (!main || !IsWindow(main))
            return NativeCommandResult::Failure("ACAPI_GetMainWindow returned null or invalid HWND");

        GS::Int32 x = -1, y = -1;
        params.Get("x", x);
        params.Get("y", y);

        POINT pt = {};
        bool defaulted = false;
        if (x < 0 || y < 0) {
            RECT cr = {};
            GetClientRect(main, &cr);
            pt.x = (cr.left + cr.right) / 2;
            pt.y = (cr.top + cr.bottom) / 2;
            ClientToScreen(main, &pt);
            defaulted = true;
        } else {
            pt.x = static_cast<LONG>(x);
            pt.y = static_cast<LONG>(y);
        }

        GS::ObjectState point;
        point.Add("x", static_cast<GS::Int32>(pt.x));
        point.Add("y", static_cast<GS::Int32>(pt.y));
        point.Add("defaulted", defaulted);

        GS::ObjectState os;
        os.Add("point", point);

        // Which document window actually answered, and whether it is the one the
        // caller asked for. See the block above for why this reports rather than
        // acts.
        GS::UniString target ("any");
        params.Get("target", target);
        os.Add("target", target);

        API_WindowInfo wi = {};
        if (ACAPI_Window_GetCurrentWindow(&wi) == NoError) {
            os.Add("windowType", static_cast<GS::Int32>(wi.typeID));
            const bool isPlan = (wi.typeID == APIWind_FloorPlanID);
            const bool is3D   = (wi.typeID == APIWind_3DModelID);
            os.Add("isPlanWindow", isPlan);
            os.Add("is3DWindow", is3D);
            const bool matched = (target == "any") ||
                                 (target == "plan"    && isPlan) ||
                                 (target == "model3d" && is3D);
            os.Add("targetMatched", matched);
            if (!matched)
                os.Add("note", GS::UniString("the front window is not the requested target - "
                                             "bring it forward and run again; nothing here "
                                             "switches windows on your behalf"));
        }

        os.Add("strictChain", ChainState(DescendChain(main, pt, false)));
        os.Add("hitChain",    ChainState(DescendChain(main, pt, true)));
        // Archicad's own tree, with our overlay taken out of the picture. This is
        // the one to resolve the canvas from; the other two answer Q3.
        os.Add("hostChain",   ChainState(DescendChain(main, pt, false, true)));

        POINT clientPt = pt;
        ScreenToClient(main, &clientPt);
        HWND real = RealChildWindowFromPoint(main, clientPt);
        if (real != nullptr)
            os.Add("realChild", WindowStateOf(real));

        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.ProbeOverlayShow {parent?, x, y, w, h, cover?} -> { ok, ... }
//
// `parent` is an HWND from ProbeWindowAt / EnumChildWindows; x/y are in THAT
// window's client coordinates. `cover` ignores x/y/w/h and fills the parent's
// client rect exactly — the interesting case, because it is what a real overlay
// would do.
// ---------------------------------------------------------------------------
class ProbeOverlayShowCommand : public MainThreadCommand {
public:
    GS::String GetName() const override { return "ProbeOverlayShow"; }

    NativeCommandResult ExecuteNative(const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        po::DestroyAll();

        HWND main = ACAPI_GetMainWindow();
        if (!main || !IsWindow(main))
            return NativeCommandResult::Failure("ACAPI_GetMainWindow returned null or invalid HWND");

        HWND parent = ResolveHwndParam(params, "parent");
        const bool parentRequested = params.Contains("parent");
        if (parentRequested && parent == nullptr)
            return NativeCommandResult::Failure("the 'parent' hwnd is not a live window — re-run ProbeWindowAt, "
                                                "the tree changes when documents open and close");
        if (parent == nullptr)
            parent = main;

        GS::Int32 x = 100, y = 100, w = 400, h = 300;
        params.Get("x", x);
        params.Get("y", y);
        params.Get("w", w);
        params.Get("h", h);

        bool cover = false;
        params.Get("cover", cover);
        if (cover) {
            // With `coverHwnd`, match THAT window's rect exactly (mapped into the
            // parent's client space) — the honest test for "a sibling laid over
            // the canvas". Without it, fill the parent's whole client area.
            HWND coverTarget = ResolveHwndParam(params, "coverHwnd");
            RECT src = {};
            if (coverTarget != nullptr) {
                GetWindowRect(coverTarget, &src);
                MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<LPPOINT>(&src), 2);
            } else {
                GetClientRect(parent, &src);
            }
            x = static_cast<GS::Int32>(src.left);
            y = static_cast<GS::Int32>(src.top);
            w = static_cast<GS::Int32>(src.right - src.left);
            h = static_cast<GS::Int32>(src.bottom - src.top);
        }

        // The paint counters reset inside po::Create — they belong to the window,
        // and a caller resetting them separately could only get it wrong.
        po::Style style;
        style.layered = false;
        params.Get("layered", style.layered);

        GS::Int32 alpha = 255;
        params.Get("alpha", alpha);
        style.alpha = static_cast<int>(alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha));

        style.hatch = false;
        params.Get("hatch", style.hatch);

        const RECT rect = { static_cast<LONG>(x), static_cast<LONG>(y),
                            static_cast<LONG>(x + w), static_cast<LONG>(y + h) };
        // The canvas is what the transform is expressed in, so it must be passed
        // explicitly — GetParent(overlay) is the document window. With `cover`
        // and a coverHwnd that IS the canvas; without one, the parent is being
        // covered directly and is therefore the reference window itself.
        HWND canvasForTransform = ResolveHwndParam(params, "coverHwnd");
        if (canvasForTransform == nullptr)
            canvasForTransform = parent;
        HWND overlay = po::Create(parent, canvasForTransform, rect, style);
        if (!overlay)
            return NativeCommandResult::Failure(GS::UniString::Printf(
                "CreateWindowEx failed (err %lu)",
                static_cast<unsigned long>(GetLastError())));

        RECT screen = {};
        GetWindowRect(overlay, &screen);

        GS::ObjectState os;
        os.Add("overlay", WindowStateOf(overlay));
        os.Add("parent", WindowStateOf(parent));
        os.Add("screenRect", RectState(screen));
        os.Add("covered", cover);
        os.Add("layered", style.layered);
        os.Add("alpha", static_cast<GS::Int32>(style.alpha));
        os.Add("hatch", style.hatch);
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.ProbeOverlayHide {restoreStyle?} -> { ok, wasVisible, paintCount,
//                                           styleRestored }
//
// ⚠️ `restoreStyle` defaults to TRUE — cleanup must stay the safe default — but
// it exists because tearing the overlay down and un-doing the clip experiment are
// two different intentions that were fused into one verb, and that silently
// invalidated a measurement: 'show' and 'watch' begin by hiding any previous
// overlay, which put the canvas's WS_CLIPSIBLINGS back before a single pixel was
// sampled. The clip was applied, reverted, and then measured as if it were still
// on. Pass false when hiding only to start clean.
// ---------------------------------------------------------------------------
class ProbeOverlayHideCommand : public MainThreadCommand {
public:
    GS::String GetName() const override { return "ProbeOverlayHide"; }

    NativeCommandResult ExecuteNative(const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        const bool wasVisible = (po::Current() != nullptr);
        const bool hadStyle   = (s_styledWindow != nullptr);

        bool restoreStyle = true;
        params.Get("restoreStyle", restoreStyle);

        po::DestroyAll();
        if (restoreStyle)
            RestoreStyledWindow();

        GS::ObjectState os;
        os.Add("wasVisible", wasVisible);
        os.Add("paintCount", static_cast<GS::Int64>(po::PaintCount()));
        os.Add("styleRestored", hadStyle && restoreStyle);
        os.Add("styleStillSet", s_styledWindow != nullptr);
        return os;
    }
};

// Sample what is ACTUALLY on the glass inside the overlay's rect.
//
// ⚠️ This is the measurement the probe was missing, and its absence produced a
// wrong conclusion. paintCount says only how often WE painted. It cannot
// distinguish "our rectangle is on screen" from "we painted once and Archicad
// has since drawn straight over the same pixels" — and those are opposite
// diagnoses. Reading the screen DC answers the question the user was being asked
// to answer by eye, faster than the eye can, and without depending on catching
// it in time.
//
// Five points, not one: an overlay half-covered by a redraw that starts at the
// canvas origin is a real outcome and a single centre sample would call it gone.
GS::ObjectState SampleOverlayPixels(HWND overlay)
{
    GS::ObjectState os;
    RECT r = {};
    GetWindowRect(overlay, &r);

    const LONG w = r.right - r.left;
    const LONG h = r.bottom - r.top;
    // Land inside the solid band the overlay draws in BOTH modes, and not on its
    // boundary pixel. Kept in step with OverlayWindow's BAND by being comfortably
    // inside it rather than by sharing the constant — a sampler that tracked the
    // draw code exactly would break the moment the drawing changed.
    constexpr LONG INSET = 7;
    const LONG insetX = (w > 8 * INSET) ? INSET : 0;
    const LONG insetY = (h > 8 * INSET) ? INSET : 0;

    const POINT pts[5] = {
        { (r.left + r.right) / 2, (r.top + r.bottom) / 2 },   // centre
        { r.left + insetX,        r.top + insetY },           // NW
        { r.right - 1 - insetX,   r.top + insetY },           // NE
        { r.left + insetX,        r.bottom - 1 - insetY },    // SW
        { r.right - 1 - insetX,   r.bottom - 1 - insetY },    // SE
    };
    static const char* names[5] = { "centre", "nw", "ne", "sw", "se" };

    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        os.Add("sampled", false);
        return os;
    }

    GS::ObjectState colours;
    GS::Int32 magenta = 0;
    GS::Int32 exact   = 0;
    for (int i = 0; i < 5; ++i) {
        COLORREF c = GetPixel(screen, pts[i].x, pts[i].y);
        if (c == CLR_INVALID) {
            colours.Add(names[i], GS::UniString("invalid"));
            continue;
        }
        const int rr = GetRValue(c), gg = GetGValue(c), bb = GetBValue(c);
        colours.Add(names[i], GS::UniString::Printf("#%02X%02X%02X", rr, gg, bb));

        if (rr == 255 && gg == 0 && bb == 255)
            ++exact;

        // ⚠️ The test has to survive ALPHA. Once the overlay is translucent the
        // pixel is our magenta BLENDED with whatever Archicad drew, so an exact
        // match would report every translucent overlay as dead — and translucent
        // is what the real overlay is. What survives blending is the SHAPE of
        // the colour: magenta is the only hue with both red and blue far above
        // green, and mixing it with an arbitrary background keeps that ordering
        // down to fairly low alpha. 24 is a wide enough margin to reject the
        // greys and near-whites of a plan drawing and narrow enough to still
        // catch a ~30% wash.
        if (rr > gg + 24 && bb > gg + 24)
            ++magenta;
    }
    ReleaseDC(nullptr, screen);

    os.Add("sampled", true);
    os.Add("colours", colours);
    os.Add("magentaPoints", magenta);
    os.Add("exactPoints", exact);
    return os;
}

// ---------------------------------------------------------------------------
// EvP.ProbeOverlayState {} -> { ok, exists, screenRect, parent, paintCount,
//                               pixels{ colours, magentaPoints }, ... }
// ---------------------------------------------------------------------------
class ProbeOverlayStateCommand : public MainThreadCommand {
public:
    GS::String GetName() const override { return "ProbeOverlayState"; }

    NativeCommandResult ExecuteNative(const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        HWND overlay = po::Current();
        const bool exists = (overlay != nullptr);
        os.Add("exists", exists);
        os.Add("paintCount", static_cast<GS::Int64>(po::PaintCount()));

        if (po::LastPaintTick() != 0) {
            os.Add("lastPaintMsAgo",
                   static_cast<GS::Int64>(GetTickCount() - po::LastPaintTick()));
        }

        if (exists) {
            RECT screen = {};
            GetWindowRect(overlay, &screen);
            os.Add("screenRect", RectState(screen));
            os.Add("overlay", WindowStateOf(overlay));
            os.Add("pixels", SampleOverlayPixels(overlay));

            // Optionally force a repaint and re-sample, which separates "we
            // cannot draw there at all" from "we draw and are then overdrawn".
            bool repaint = false;
            params.Get("repaint", repaint);
            if (repaint) {
                const LONG before = po::PaintCount();
                InvalidateRect(overlay, nullptr, TRUE);
                UpdateWindow(overlay);
                os.Add("repaintPainted", po::PaintCount() != before);
                os.Add("pixelsAfterRepaint", SampleOverlayPixels(overlay));
            }

            HWND parent = GetParent(overlay);
            if (parent != nullptr && IsWindow(parent)) {
                os.Add("parent", WindowStateOf(parent));
                RECT pc = {};
                GetClientRect(parent, &pc);
                os.Add("parentClientRect", RectState(pc));
            }
        }

        if (s_styledWindow != nullptr && IsWindow(s_styledWindow))
            os.Add("styledWindow", WindowStateOf(s_styledWindow));

        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.ProbeSetClipSiblings {hwnd, enable, clipSiblings?, clipChildren?}
//                                         -> { ok, before, after }
//
// The experiment for the one known risk in the child-window approach. Confirmed
// live: Archicad's canvas (DGUserItemClass) carries neither WS_CLIPSIBLINGS nor
// WS_CLIPCHILDREN, so its painting is clipped around NEITHER a sibling overlay
// nor a child one. Which bit is needed depends on how the overlay attached:
//
//   attach=sibling -> WS_CLIPSIBLINGS on the canvas
//   attach=child   -> WS_CLIPCHILDREN on the canvas
//
// Both bits go on the SAME window, which is why one command covers both. The
// original style is remembered so ProbeOverlayHide restores it.
//
// Deliberately opt-in and single-target: modifying a host window's style is not
// something to do casually, and leaving it modified is worse than not trying.
// ---------------------------------------------------------------------------
class ProbeSetClipSiblingsCommand : public MainThreadCommand {
public:
    GS::String GetName() const override { return "ProbeSetClipSiblings"; }

    NativeCommandResult ExecuteNative(const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        HWND target = ResolveHwndParam(params, "hwnd");
        if (target == nullptr)
            return NativeCommandResult::Failure("'hwnd' is missing or not a live window");

        bool enable = true;
        params.Get("enable", enable);

        // Default to WS_CLIPSIBLINGS alone, which is the sibling-attach case.
        bool clipSiblings = true, clipChildren = false;
        params.Get("clipSiblings", clipSiblings);
        params.Get("clipChildren", clipChildren);

        LONG bits = 0;
        if (clipSiblings) bits |= WS_CLIPSIBLINGS;
        if (clipChildren) bits |= WS_CLIPCHILDREN;
        if (bits == 0)
            return NativeCommandResult::Failure("neither clipSiblings nor clipChildren was requested — "
                                                "nothing to change");

        const LONG before = static_cast<LONG>(GetWindowLongPtrW(target, GWL_STYLE));

        if (s_styledWindow != nullptr && s_styledWindow != target)
            RestoreStyledWindow();

        LONG after = enable ? (before | bits) : (before & ~bits);

        // Only claim to have modified a host window if the bits actually changed.
        // The first live run asked for WS_CLIPSIBLINGS on a window that already
        // had it: nothing changed, yet the state report warned "a style bit is
        // set on one of Archicad's windows, do not close Archicad" — a scary
        // message about a no-op.
        if (after != before && s_styledWindow == nullptr) {
            s_styledWindow   = target;
            s_styledOriginal = before;
        }

        SetWindowLongPtrW(target, GWL_STYLE, after);
        SetWindowPos(target, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
                     | SWP_FRAMECHANGED);
        after = static_cast<LONG>(GetWindowLongPtrW(target, GWL_STYLE));

        GS::ObjectState os;
        os.Add("target", WindowStateOf(target));
        os.Add("before", GS::UniString::Printf("0x%08lX", static_cast<unsigned long>(before)));
        os.Add("after",  GS::UniString::Printf("0x%08lX", static_cast<unsigned long>(after)));
        os.Add("enabled", enable);
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.ProbeForceRedraw {mode?} -> { ok, mode, acapiErr?, invalidated? }
//
// Take the human out of the measurement loop.
//
// Every overlay result so far depended on the user panning and zooming while a
// sampler ran, which makes "did it survive" partly a question about how
// vigorously somebody dragged a mouse. `ACAPI_View_Redraw` forces a genuine plan
// redraw on demand, so a sweep of configurations can be measured under an
// identical, repeatable provocation.
//
// mode:
//   "acapi"      ACAPI_View_Redraw — Archicad's own redraw, the real thing
//   "invalidate" RedrawWindow on the canvas with RDW_NOCHILDREN — repaints THEIR
//                window without repainting ours, which is the discriminating
//                case: RDW_ALLCHILDREN would repaint the overlay too and hide
//                exactly the failure being looked for
//   "both"       (default)
// ---------------------------------------------------------------------------
class ProbeForceRedrawCommand : public MainThreadCommand {
public:
    GS::String GetName() const override { return "ProbeForceRedraw"; }

    NativeCommandResult ExecuteNative(const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::UniString mode = "both";
        params.Get("mode", mode);

        GS::ObjectState os;
        os.Add("mode", mode);

        if (mode == "acapi" || mode == "both") {
            const GSErrCode err = ACAPI_View_Redraw();
            os.Add("acapiErr", static_cast<GS::Int32>(err));
            if (err != NoError)
                os.Add("acapiError", EVP_ACAPI_FAIL("ACAPI_View_Redraw", err,
                                                    "forcing a plan redraw for the overlay probe"));
        }

        if (mode == "invalidate" || mode == "both") {
            HWND canvas = ResolveHwndParam(params, "hwnd");
            if (canvas != nullptr) {
                RedrawWindow(canvas, nullptr, nullptr,
                             RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_NOCHILDREN);
                os.Add("invalidated", true);
            } else {
                os.Add("invalidated", false);
            }
        }

        return os;
    }
};

const NativeCommandRegistration kPlanOverlayCommandRegistrations[] = {
    { "EnumChildWindows", &MakeRegisteredNativeCommand<EnumChildWindowsCommand>, false,
      R"json({"type":"object","properties":{},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"mainWindow":{"type":"object","properties":{"hwnd":{"type":"integer"},"parent":{"type":"integer"},"class":{"type":"string"},"text":{"type":"string"},"ctrlId":{"type":"integer"},"rect":{"type":"object","properties":{"left":{"type":"integer"},"top":{"type":"integer"},"right":{"type":"integer"},"bottom":{"type":"integer"}},"additionalProperties":false,"required":["left","top","right","bottom"]},"style":{"type":"string"},"exStyle":{"type":"string"},"visible":{"type":"boolean"}},"additionalProperties":false,"required":["hwnd","parent","class","text","ctrlId","rect","style","exStyle","visible"]},"windows":{"type":"array","items":{"type":"object","properties":{"hwnd":{"type":"integer"},"parent":{"type":"integer"},"class":{"type":"string"},"text":{"type":"string"},"ctrlId":{"type":"integer"},"rect":{"type":"object","properties":{"left":{"type":"integer"},"top":{"type":"integer"},"right":{"type":"integer"},"bottom":{"type":"integer"}},"additionalProperties":false,"required":["left","top","right","bottom"]},"style":{"type":"string"},"exStyle":{"type":"string"},"visible":{"type":"boolean"}},"additionalProperties":false,"required":["hwnd","parent","class","text","ctrlId","rect","style","exStyle","visible"]}},"count":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["mainWindow","windows","count"]})json" },
    { "ProbeWindowAt", &MakeRegisteredNativeCommand<ProbeWindowAtCommand>, false,
      R"json({"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"},"target":{"type":"string","enum":["any","plan","model3d"]}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"point":{"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"},"defaulted":{"type":"boolean"}},"additionalProperties":false,"required":["x","y","defaulted"]},"target":{"type":"string","enum":["any","plan","model3d"]},"windowType":{"type":"integer"},"isPlanWindow":{"type":"boolean"},"is3DWindow":{"type":"boolean"},"targetMatched":{"type":"boolean"},"note":{"type":"string"},"strictChain":{"type":"array","items":{"type":"object","properties":{"hwnd":{"type":"integer"},"parent":{"type":"integer"},"class":{"type":"string"},"text":{"type":"string"},"ctrlId":{"type":"integer"},"rect":{"type":"object","properties":{"left":{"type":"integer"},"top":{"type":"integer"},"right":{"type":"integer"},"bottom":{"type":"integer"}},"additionalProperties":false,"required":["left","top","right","bottom"]},"style":{"type":"string"},"exStyle":{"type":"string"},"visible":{"type":"boolean"}},"additionalProperties":false,"required":["hwnd","parent","class","text","ctrlId","rect","style","exStyle","visible"]}},"hitChain":{"type":"array","items":{"type":"object","properties":{"hwnd":{"type":"integer"},"parent":{"type":"integer"},"class":{"type":"string"},"text":{"type":"string"},"ctrlId":{"type":"integer"},"rect":{"type":"object","properties":{"left":{"type":"integer"},"top":{"type":"integer"},"right":{"type":"integer"},"bottom":{"type":"integer"}},"additionalProperties":false,"required":["left","top","right","bottom"]},"style":{"type":"string"},"exStyle":{"type":"string"},"visible":{"type":"boolean"}},"additionalProperties":false,"required":["hwnd","parent","class","text","ctrlId","rect","style","exStyle","visible"]}},"hostChain":{"type":"array","items":{"type":"object","properties":{"hwnd":{"type":"integer"},"parent":{"type":"integer"},"class":{"type":"string"},"text":{"type":"string"},"ctrlId":{"type":"integer"},"rect":{"type":"object","properties":{"left":{"type":"integer"},"top":{"type":"integer"},"right":{"type":"integer"},"bottom":{"type":"integer"}},"additionalProperties":false,"required":["left","top","right","bottom"]},"style":{"type":"string"},"exStyle":{"type":"string"},"visible":{"type":"boolean"}},"additionalProperties":false,"required":["hwnd","parent","class","text","ctrlId","rect","style","exStyle","visible"]}},"realChild":{"type":"object","properties":{"hwnd":{"type":"integer"},"parent":{"type":"integer"},"class":{"type":"string"},"text":{"type":"string"},"ctrlId":{"type":"integer"},"rect":{"type":"object","properties":{"left":{"type":"integer"},"top":{"type":"integer"},"right":{"type":"integer"},"bottom":{"type":"integer"}},"additionalProperties":false,"required":["left","top","right","bottom"]},"style":{"type":"string"},"exStyle":{"type":"string"},"visible":{"type":"boolean"}},"additionalProperties":false,"required":["hwnd","parent","class","text","ctrlId","rect","style","exStyle","visible"]}},"additionalProperties":false,"required":["point","target","strictChain","hitChain","hostChain"]})json" },
    { "ProbeOverlayShow", &MakeRegisteredNativeCommand<ProbeOverlayShowCommand>, false,
      R"json({"type":"object","properties":{"parent":{"type":"integer"},"x":{"type":"integer"},"y":{"type":"integer"},"w":{"type":"integer"},"h":{"type":"integer"},"cover":{"type":"boolean"},"coverHwnd":{"type":"integer"},"layered":{"type":"boolean"},"alpha":{"type":"integer","minimum":0,"maximum":255},"hatch":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"overlay":{"type":"object","properties":{"hwnd":{"type":"integer"},"parent":{"type":"integer"},"class":{"type":"string"},"text":{"type":"string"},"ctrlId":{"type":"integer"},"rect":{"type":"object","properties":{"left":{"type":"integer"},"top":{"type":"integer"},"right":{"type":"integer"},"bottom":{"type":"integer"}},"additionalProperties":false,"required":["left","top","right","bottom"]},"style":{"type":"string"},"exStyle":{"type":"string"},"visible":{"type":"boolean"}},"additionalProperties":false,"required":["hwnd","parent","class","text","ctrlId","rect","style","exStyle","visible"]},"parent":{"type":"object","properties":{"hwnd":{"type":"integer"},"parent":{"type":"integer"},"class":{"type":"string"},"text":{"type":"string"},"ctrlId":{"type":"integer"},"rect":{"type":"object","properties":{"left":{"type":"integer"},"top":{"type":"integer"},"right":{"type":"integer"},"bottom":{"type":"integer"}},"additionalProperties":false,"required":["left","top","right","bottom"]},"style":{"type":"string"},"exStyle":{"type":"string"},"visible":{"type":"boolean"}},"additionalProperties":false,"required":["hwnd","parent","class","text","ctrlId","rect","style","exStyle","visible"]},"screenRect":{"type":"object","properties":{"left":{"type":"integer"},"top":{"type":"integer"},"right":{"type":"integer"},"bottom":{"type":"integer"}},"additionalProperties":false,"required":["left","top","right","bottom"]},"covered":{"type":"boolean"},"layered":{"type":"boolean"},"alpha":{"type":"integer","minimum":0,"maximum":255},"hatch":{"type":"boolean"}},"additionalProperties":false,"required":["overlay","parent","screenRect","covered","layered","alpha","hatch"]})json" },
    { "ProbeOverlayHide", &MakeRegisteredNativeCommand<ProbeOverlayHideCommand>, false,
      R"json({"type":"object","properties":{"restoreStyle":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"wasVisible":{"type":"boolean"},"paintCount":{"type":"integer","minimum":0},"styleRestored":{"type":"boolean"},"styleStillSet":{"type":"boolean"}},"additionalProperties":false,"required":["wasVisible","paintCount","styleRestored","styleStillSet"]})json" },
    { "ProbeOverlayState", &MakeRegisteredNativeCommand<ProbeOverlayStateCommand>, false,
      R"json({"type":"object","properties":{"repaint":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"exists":{"type":"boolean"},"paintCount":{"type":"integer","minimum":0},"lastPaintMsAgo":{"type":"integer","minimum":0},"screenRect":{"type":"object","properties":{"left":{"type":"integer"},"top":{"type":"integer"},"right":{"type":"integer"},"bottom":{"type":"integer"}},"additionalProperties":false,"required":["left","top","right","bottom"]},"overlay":{"type":"object","properties":{"hwnd":{"type":"integer"},"parent":{"type":"integer"},"class":{"type":"string"},"text":{"type":"string"},"ctrlId":{"type":"integer"},"rect":{"type":"object","properties":{"left":{"type":"integer"},"top":{"type":"integer"},"right":{"type":"integer"},"bottom":{"type":"integer"}},"additionalProperties":false,"required":["left","top","right","bottom"]},"style":{"type":"string"},"exStyle":{"type":"string"},"visible":{"type":"boolean"}},"additionalProperties":false,"required":["hwnd","parent","class","text","ctrlId","rect","style","exStyle","visible"]},"pixels":{"type":"object","properties":{"sampled":{"type":"boolean"},"colours":{"type":"object","properties":{"centre":{"type":"string"},"nw":{"type":"string"},"ne":{"type":"string"},"sw":{"type":"string"},"se":{"type":"string"}},"additionalProperties":false},"magentaPoints":{"type":"integer","minimum":0,"maximum":5},"exactPoints":{"type":"integer","minimum":0,"maximum":5}},"additionalProperties":false,"required":["sampled"]},"repaintPainted":{"type":"boolean"},"pixelsAfterRepaint":{"type":"object","properties":{"sampled":{"type":"boolean"},"colours":{"type":"object","properties":{"centre":{"type":"string"},"nw":{"type":"string"},"ne":{"type":"string"},"sw":{"type":"string"},"se":{"type":"string"}},"additionalProperties":false},"magentaPoints":{"type":"integer","minimum":0,"maximum":5},"exactPoints":{"type":"integer","minimum":0,"maximum":5}},"additionalProperties":false,"required":["sampled"]},"parent":{"type":"object","properties":{"hwnd":{"type":"integer"},"parent":{"type":"integer"},"class":{"type":"string"},"text":{"type":"string"},"ctrlId":{"type":"integer"},"rect":{"type":"object","properties":{"left":{"type":"integer"},"top":{"type":"integer"},"right":{"type":"integer"},"bottom":{"type":"integer"}},"additionalProperties":false,"required":["left","top","right","bottom"]},"style":{"type":"string"},"exStyle":{"type":"string"},"visible":{"type":"boolean"}},"additionalProperties":false,"required":["hwnd","parent","class","text","ctrlId","rect","style","exStyle","visible"]},"parentClientRect":{"type":"object","properties":{"left":{"type":"integer"},"top":{"type":"integer"},"right":{"type":"integer"},"bottom":{"type":"integer"}},"additionalProperties":false,"required":["left","top","right","bottom"]},"styledWindow":{"type":"object","properties":{"hwnd":{"type":"integer"},"parent":{"type":"integer"},"class":{"type":"string"},"text":{"type":"string"},"ctrlId":{"type":"integer"},"rect":{"type":"object","properties":{"left":{"type":"integer"},"top":{"type":"integer"},"right":{"type":"integer"},"bottom":{"type":"integer"}},"additionalProperties":false,"required":["left","top","right","bottom"]},"style":{"type":"string"},"exStyle":{"type":"string"},"visible":{"type":"boolean"}},"additionalProperties":false,"required":["hwnd","parent","class","text","ctrlId","rect","style","exStyle","visible"]}},"additionalProperties":false,"required":["exists","paintCount"]})json" },
    { "ProbeSetClipSiblings", &MakeRegisteredNativeCommand<ProbeSetClipSiblingsCommand>, false,
      R"json({"type":"object","properties":{"hwnd":{"type":"integer"},"enable":{"type":"boolean"},"clipSiblings":{"type":"boolean"},"clipChildren":{"type":"boolean"}},"additionalProperties":false,"required":["hwnd"]})json",
      R"json({"type":"object","properties":{"target":{"type":"object","properties":{"hwnd":{"type":"integer"},"parent":{"type":"integer"},"class":{"type":"string"},"text":{"type":"string"},"ctrlId":{"type":"integer"},"rect":{"type":"object","properties":{"left":{"type":"integer"},"top":{"type":"integer"},"right":{"type":"integer"},"bottom":{"type":"integer"}},"additionalProperties":false,"required":["left","top","right","bottom"]},"style":{"type":"string"},"exStyle":{"type":"string"},"visible":{"type":"boolean"}},"additionalProperties":false,"required":["hwnd","parent","class","text","ctrlId","rect","style","exStyle","visible"]},"before":{"type":"string"},"after":{"type":"string"},"enabled":{"type":"boolean"}},"additionalProperties":false,"required":["target","before","after","enabled"]})json" },
    { "ProbeForceRedraw", &MakeRegisteredNativeCommand<ProbeForceRedrawCommand>, false,
      R"json({"type":"object","properties":{"mode":{"type":"string","enum":["acapi","invalidate","both"]},"hwnd":{"type":"integer"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"mode":{"type":"string","enum":["acapi","invalidate","both"]},"acapiErr":{"type":"integer"},"acapiError":{"type":"string"},"invalidated":{"type":"boolean"}},"additionalProperties":false,"required":["mode"]})json" },
};

} // namespace

// ---------------------------------------------------------------------------
// Domain registrations
// ---------------------------------------------------------------------------
NativeCommandRegistrations GetPlanOverlayCommandRegistrations ()
{
    return MakeRegistrationView (kPlanOverlayCommandRegistrations);
}

// ---------------------------------------------------------------------------
// Unload cleanup — called from FreeData
//
// A probe is allowed to be crude; it is not allowed to crash the host on exit.
// Two things must be gone before this DLL unloads: any window whose WndProc is
// OverlayWndProc (Archicad destroys its own window tree at quit and would call
// into freed code), and the class registration that names that address. Style
// bits set on Archicad's own windows are put back here too, so forgetting
// action='hide' costs nothing.
// ---------------------------------------------------------------------------
void ShutdownPlanOverlay()
{
    RestoreStyledWindow();
    planoverlay::Shutdown();
}

// ---------------------------------------------------------------------------
// JSON port install — reads only, install EnumChildWindows for the Tapir pathway
// ---------------------------------------------------------------------------
GSErrCode InstallPlanOverlayJsonCommands()
{
    return ACAPI_AddOnAddOnCommunication_InstallAddOnCommandHandler(
        GS::NewOwned<RegisteredNativeCommand<EnumChildWindowsCommand>>(kPlanOverlayCommandRegistrations[0]));
}

} // namespace geomsrv
