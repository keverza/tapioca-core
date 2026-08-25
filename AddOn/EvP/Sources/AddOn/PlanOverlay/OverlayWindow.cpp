#include "APIEnvir.h"
#include "ACAPinc.h"

#include "PlanOverlay/OverlayWindow.hpp"
#include "Annotation/GdiPainter.hpp"
#include "PlanOverlay/PlanTransformMath.hpp"

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace geomsrv {
namespace planoverlay {

namespace {

constexpr const wchar_t* OVERLAY_CLASS = L"EvP_PlanOverlayProbe_Window";

// The colour keyed to fully transparent in hatch mode. Not black and not white:
// it must be a colour our own drawing never uses, or we would punch holes in it.
constexpr COLORREF KEY_COLOUR = RGB (1, 1, 1);
constexpr COLORREF INK_COLOUR = RGB (255, 0, 255);

// Width of the solid band inside the edge, and of the centre cross. The pixel
// sampler probes 4 corner insets and the centre, so both must land on drawn ink
// in BOTH draw modes or the two modes would not be comparable.
constexpr int BAND = 14;

constexpr UINT_PTR TRACK_TIMER_ID = 0xE7B1;
constexpr UINT TRACK_FAST_INTERVAL_MS = 16;
// Stable views still poll near 30 Hz so motion onset is observed promptly.
constexpr UINT TRACK_STABLE_INTERVAL_MS = 33;

HWND s_overlay = nullptr;
Owner s_owner = Owner::None;

// ⚠️ The CANVAS, remembered — NOT GetParent(overlay).
//
// The overlay is a SIBLING of the canvas, so its parent is the floor-plan
// DOCUMENT window, whose client rect is both larger and differently placed. The
// first version derived the reference points and the draw shift from the parent,
// which put every pinned coordinate off by the canvas's inset inside the
// document window — the geometry tracked the view correctly and sat in the wrong
// place, which reads as "it did not pin to the origin".
HWND s_canvas = nullptr;

bool s_classRegistered = false;
Style s_style;

// Every overlay we have created and not destroyed ourselves. s_overlay alone is
// not enough to clean up with: Archicad destroys the overlay for us when its
// parent document window closes (a storey switch does it), after which a NEW one
// can exist while the old handle is merely stale. On unload the only safe
// question is "did we create it", not "is it the current one".
std::vector<HWND> s_created;

volatile LONG s_paintCount = 0;
DWORD s_lastPaintTick = 0;

std::vector<Polyline> s_geometry;
std::shared_ptr<const annotation::Frame> s_annotationFrame;
bool s_annotationLayerClaimed = false;
Transform s_transform;
PlanTransform s_paintedTransform;
PlanTransform s_acceptedTransform;
TrackStats s_stats;
UINT s_requestedIntervalMs = TRACK_STABLE_INTERVAL_MS;
bool s_fastTimer = false;
ULONGLONG s_lastActivityTick = 0;

bool HasOverlayClass (HWND hwnd)
{
    wchar_t cls[256] = {};
    GetClassNameW (hwnd, cls, 255);
    return wcscmp (cls, OVERLAY_CLASS) == 0;
}

// This DLL's module handle — NOT the host EXE's. The class and the window must
// belong to the module that owns the WndProc, or UnregisterClassW cannot take
// the class down at unload and a stale procedure address survives in Archicad's
// class table.
HMODULE OwnModule ();

// -------------------------------------------------------------------------
// The transform
// -------------------------------------------------------------------------
//
// Derived from three corners and a repeated first corner, rather than assembled
// from the zoom box, drawing scale and tranmat.
//
// ⚠️ Deliberate. Reconstructing the projection from its parts means getting the
// model box, the drawing scale, the transformation matrix AND their interaction
// right, and being wrong in any one of them looks exactly like being wrong in
// the others. Asking Archicad where two reference points land makes the
// projection self-evident: exact by construction, 4 ACAPI calls per observation
// rather than per vertex (the rule that matters, §15.1), and when it is wrong
// the reference pairs are in the log to say why.
//
// ⚠️ The direction matters: screen -> model (PointToCoord), NOT model -> screen.
// API_Point is a pair of SHORTs. Fixed model reference points would project past
// ±32767 at any serious zoom and silently overflow — a projection that works on
// the test file and breaks when somebody zooms in. Screen points are bounded by
// the window, always, so the inputs cannot overflow and the outputs are doubles.
struct TransformObservation {
    PlanTransform affine;
    Transform reported;
};

bool SampleModelCoord (long x, long y, PlanPoint& model)
{
    API_Point point = {};
    point.h = static_cast<short> (x);
    point.v = static_cast<short> (y);
    API_Coord coord = {};
    if (ACAPI_View_PointToCoord (&point, &coord) != NoError)
        return false;
    model = { coord.x, coord.y };
    return true;
}

TransformObservation DeriveTransform ()
{
    TransformObservation observation;

    if (Current () == nullptr || s_canvas == nullptr || !IsWindow (s_canvas))
        return observation;

    RECT cr = {};
    GetClientRect (s_canvas, &cr);
    const double physicalWidth = double (cr.right - cr.left);
    const double physicalHeight = double (cr.bottom - cr.top);
    const UINT dpi = GetDpiForWindow (s_canvas);
    if (dpi == 0)
        return observation;
    const LogicalSampleRect sampleRect = MakeLogicalSampleRect (physicalWidth, physicalHeight, double (dpi) / 96.0);
    if (!sampleRect.valid || sampleRect.right > 32000 || sampleRect.bottom > 32000)
        return observation;

    PlanPoint topLeft, topRight, bottomLeft, topLeftAgain;
    if (!SampleModelCoord (0, 0, topLeft) || !SampleModelCoord (sampleRect.right, 0, topRight) ||
        !SampleModelCoord (0, sampleRect.bottom, bottomLeft) || !SampleModelCoord (0, 0, topLeftAgain))
        return observation;

    observation.affine = ObservePlanTransform (sampleRect, topLeft, topRight, bottomLeft, topLeftAgain);
    Transform& t = observation.reported;
    t.valid = observation.affine.valid;
    t.scaleX = observation.affine.xx;
    t.xFromY = observation.affine.xy;
    t.yFromX = observation.affine.yx;
    t.scaleY = observation.affine.yy;
    t.offX = observation.affine.offsetX;
    t.offY = observation.affine.offsetY;
    t.refModelA = { topLeft.x, topLeft.y };
    t.refModelB = { topRight.x, topRight.y };
    t.refPointA = { 0.0, 0.0 };
    t.refPointB = { double (sampleRect.right), 0.0 };
    t.canvasW = physicalWidth;
    t.canvasH = physicalHeight;
    t.impliedW = double (sampleRect.right);
    t.impliedH = double (sampleRect.bottom);
    t.dpiX = sampleRect.dpiScale;
    t.dpiY = sampleRect.dpiScale;
    t.dpiApplied = true;
    return observation;
}

std::vector<PlanPoint> PaintedGeometry ()
{
    std::vector<PlanPoint> points;
    for (const Polyline& polyline : s_geometry) {
        for (const Point2& point : polyline)
            points.push_back ({ point.x, point.y });
    }
    if (s_annotationFrame) {
        for (const annotation::Primitive& primitive : s_annotationFrame->primitives) {
            if (!annotation::IsDrawable (primitive) || primitive.kind == annotation::PrimitiveKind::Element)
                continue;
            for (const annotation::Point3& point : primitive.points)
                points.push_back ({ point.x, point.y });
        }
    }
    return points;
}

// -------------------------------------------------------------------------
// Painting
// -------------------------------------------------------------------------

void PaintTestPattern (HDC hdc, const RECT& rc)
{
    HBRUSH ink = CreateSolidBrush (INK_COLOUR);

    if (s_style.hatch) {
        HBRUSH key = CreateSolidBrush (KEY_COLOUR);
        FillRect (hdc, &rc, key);
        DeleteObject (key);

        RECT band = rc;
        for (int i = 0; i < BAND; ++i) {
            FrameRect (hdc, &band, ink);
            InflateRect (&band, -1, -1);
        }

        const LONG cx = (rc.left + rc.right) / 2;
        const LONG cy = (rc.top + rc.bottom) / 2;
        RECT h = { rc.left, cy - BAND / 2, rc.right, cy + BAND / 2 };
        RECT v = { cx - BAND / 2, rc.top, cx + BAND / 2, rc.bottom };
        FillRect (hdc, &h, ink);
        FillRect (hdc, &v, ink);
    }
    else {
        FillRect (hdc, &rc, ink);
        HBRUSH border = CreateSolidBrush (RGB (0, 0, 0));
        FrameRect (hdc, &rc, border);
        DeleteObject (border);
    }

    DeleteObject (ink);
}

void PaintGeometry (HDC hdc, HWND hwnd, const RECT& rc)
{
    // Background first: keyed out so the plan shows through everywhere we are
    // not actually drawing. Without this the geometry would sit on an opaque
    // sheet and hide the very drawing it is annotating.
    HBRUSH key = CreateSolidBrush (s_style.hatch ? KEY_COLOUR : RGB (0, 0, 0));
    FillRect (hdc, &rc, key);
    DeleteObject (key);

    if (!s_transform.valid)
        return;

    // The transform speaks in the CANVAS's client space and we are a separate
    // window over it, so the two origins differ by wherever our window sits.
    // Map through the screen rather than assuming they coincide — and against
    // the remembered canvas, never GetParent, which is the document window.
    POINT origin = { 0, 0 };
    ClientToScreen (hwnd, &origin);
    POINT canvasOrigin = { 0, 0 };
    if (s_canvas != nullptr && IsWindow (s_canvas))
        ClientToScreen (s_canvas, &canvasOrigin);
    const int shiftX = canvasOrigin.x - origin.x;
    const int shiftY = canvasOrigin.y - origin.y;

    if (s_annotationFrame) {
        annotation::Transform2D transform;
        transform.scaleX = s_transform.scaleX;
        transform.xFromY = s_transform.xFromY;
        transform.yFromX = s_transform.yFromX;
        transform.scaleY = s_transform.scaleY;
        transform.offX = s_transform.offX;
        transform.offY = s_transform.offY;
        annotation::GdiPaintOptions options;
        options.originX = shiftX;
        options.originY = shiftY;
        options.textColour = RGB (255, 255, 255);
        annotation::PaintFrameGdi (hdc, *s_annotationFrame, transform, options);
    }

    HPEN pen = CreatePen (PS_SOLID, 2, INK_COLOUR);
    HGDIOBJ oldPen = SelectObject (hdc, pen);

    for (const Polyline& poly : s_geometry) {
        if (poly.size () < 2)
            continue;
        for (size_t i = 0; i < poly.size (); ++i) {
            // lround, not a cast: truncation is toward zero, so it biases
            // opposite ways either side of the origin and shows up as the
            // geometry shifting by a pixel as it crosses.
            const int px = static_cast<int> (std::lround (s_transform.offX + poly[i].x * s_transform.scaleX +
                                                          poly[i].y * s_transform.xFromY)) +
                           shiftX;
            const int py = static_cast<int> (std::lround (s_transform.offY + poly[i].x * s_transform.yFromX +
                                                          poly[i].y * s_transform.scaleY)) +
                           shiftY;
            if (i == 0)
                MoveToEx (hdc, px, py, nullptr);
            else
                LineTo (hdc, px, py);
        }
    }

    SelectObject (hdc, oldPen);
    DeleteObject (pen);
}

bool RepositionOverCanvas (HWND overlay)
{
    HWND parent = GetParent (overlay);
    if (parent == nullptr || s_canvas == nullptr)
        return false;
    RECT wanted = {};
    GetWindowRect (s_canvas, &wanted);
    MapWindowPoints (HWND_DESKTOP, parent, reinterpret_cast<POINT*> (&wanted), 2);
    RECT current = {};
    GetWindowRect (overlay, &current);
    MapWindowPoints (HWND_DESKTOP, parent, reinterpret_cast<POINT*> (&current), 2);
    if (EqualRect (&wanted, &current))
        return false;
    SetWindowPos (overlay, HWND_TOP, wanted.left, wanted.top, wanted.right - wanted.left, wanted.bottom - wanted.top,
                  SWP_NOACTIVATE);
    return true;
}

void SetTimerCadence (HWND overlay, bool fast)
{
    if (s_fastTimer == fast)
        return;
    s_fastTimer = fast;
    const UINT interval = fast ? (std::min) (s_requestedIntervalMs, TRACK_FAST_INTERVAL_MS)
                               : (std::max) (s_requestedIntervalMs, TRACK_STABLE_INTERVAL_MS);
    SetTimer (overlay, TRACK_TIMER_ID, interval, nullptr);
    s_stats.intervalMs = interval;
}

LRESULT CALLBACK OverlayWndProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            return TRUE;

        case WM_TIMER:
            if (wp == TRACK_TIMER_ID) {
                // ⚠️ ACAPI from a WM_TIMER is legal ONLY because a WndProc runs on
                // Archicad's UI thread. This is the main thread; it is not a worker
                // and must never become one.
                ++s_stats.polls;
                const bool rectChanged = RepositionOverCanvas (hwnd);
                const TransformObservation observation = DeriveTransform ();
                if (!observation.affine.valid)
                    ++s_stats.acapiFailures;
                const std::vector<PlanPoint> geometry = PaintedGeometry ();
                const bool pixelsChanged =
                    RoundedProjectedPixelsChanged (s_paintedTransform, observation.affine, geometry);
                if (observation.affine.valid) {
                    AdoptPlanTransform (observation.affine, s_acceptedTransform);
                    s_transform = observation.reported;
                }
                if (pixelsChanged || rectChanged) {
                    ++s_stats.recomputes;
                    // Repaint only when the view actually moved — an unconditional
                    // repaint at the poll rate would burn a frame per tick drawing
                    // the identical image (§16.3).
                    //
                    // ⚠️ RDW_UPDATENOW, not a bare InvalidateRect. InvalidateRect
                    // merely marks the window dirty; the WM_PAINT is then delivered
                    // only when the message queue runs dry, and during a continuous
                    // pan Archicad's own messages never let it. The overlay would
                    // arrive whole frames late and appear to swim against the plan —
                    // which is exactly the reported "wobble" and "slight lag", one
                    // symptom rather than two.
                    RedrawWindow (hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
                }
                const ULONGLONG now = GetTickCount64 ();
                const bool torn = !observation.affine.valid && observation.affine.tearPixels > 0.25;
                if (pixelsChanged || rectChanged || torn)
                    s_lastActivityTick = now;
                SetTimerCadence (hwnd, torn || now - s_lastActivityTick < 250);
                return 0;
            }
            break;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint (hwnd, &ps);
            RECT rc;
            GetClientRect (hwnd, &rc);

            if (s_geometry.empty () && !s_annotationFrame && !s_annotationLayerClaimed)
                PaintTestPattern (hdc, rc);
            else
                PaintGeometry (hdc, hwnd, rc);

            s_paintedTransform = s_acceptedTransform;

            EndPaint (hwnd, &ps);

            InterlockedIncrement (&s_paintCount);
            ++s_stats.repaints;
            s_lastPaintTick = GetTickCount ();
            return 0;
        }
    }
    return DefWindowProcW (hwnd, msg, wp, lp);
}

HMODULE OwnModule ()
{
    static HMODULE mod = nullptr;
    if (mod == nullptr) {
        GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR> (&OverlayWndProc), &mod);
    }
    return mod;
}

} // namespace

// ---------------------------------------------------------------------------

HWND Create (HWND parent, HWND canvas, const RECT& rectInParent, const Style& style, Owner owner)
{
    DestroyAll ();
    s_style = style;
    s_canvas = canvas;

    if (!s_classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof (wc);
        wc.lpfnWndProc = OverlayWndProc;
        wc.hInstance = OwnModule ();
        wc.lpszClassName = OVERLAY_CLASS;
        wc.hbrBackground = reinterpret_cast<HBRUSH> (GetStockObject (NULL_BRUSH));
        wc.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW (&wc);
        s_classRegistered = true;
    }

    DWORD ex = WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    if (style.layered)
        ex |= WS_EX_LAYERED;

    HWND hwnd = CreateWindowExW (ex, OVERLAY_CLASS, L"EvP Probe Overlay", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                 rectInParent.left, rectInParent.top, rectInParent.right - rectInParent.left,
                                 rectInParent.bottom - rectInParent.top, parent, nullptr, OwnModule (), nullptr);

    if (hwnd == nullptr)
        return nullptr;

    if (style.layered) {
        DWORD flags = LWA_ALPHA;
        if (style.hatch)
            flags |= LWA_COLORKEY;
        SetLayeredWindowAttributes (hwnd, KEY_COLOUR, static_cast<BYTE> (style.alpha), flags);
    }
    SetWindowPos (hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    s_overlay = hwnd;
    s_owner = owner;
    s_created.push_back (hwnd);
    s_paintCount = 0;
    s_lastPaintTick = 0;
    s_stats = TrackStats {};
    s_transform = Transform {};
    s_acceptedTransform = PlanTransform {};
    s_paintedTransform = PlanTransform {};
    return hwnd;
}

void DestroyAll ()
{
    for (HWND h : s_created) {
        if (h != nullptr && IsWindow (h) && HasOverlayClass (h))
            DestroyWindow (h);
    }
    s_created.clear ();
    s_overlay = nullptr;
    s_owner = Owner::None;
    s_canvas = nullptr;
    s_stats.tracking = false;
    s_acceptedTransform = PlanTransform {};
    s_paintedTransform = PlanTransform {};
}

bool DestroyOwned (Owner owner)
{
    if (Current () == nullptr || s_owner != owner)
        return false;
    DestroyAll ();
    return true;
}

HWND Current ()
{
    return (s_overlay != nullptr && IsWindow (s_overlay)) ? s_overlay : nullptr;
}

HWND Canvas ()
{
    return Current () != nullptr && s_canvas != nullptr && IsWindow (s_canvas) ? s_canvas : nullptr;
}

Owner CurrentOwner ()
{
    return Current () != nullptr ? s_owner : Owner::None;
}

bool SetOpacity (Owner owner, int alpha)
{
    HWND const window = Current ();
    if (window == nullptr || s_owner != owner || !s_style.layered)
        return false;
    s_style.alpha = std::clamp (alpha, 0, 255);
    DWORD flags = LWA_ALPHA;
    if (s_style.hatch)
        flags |= LWA_COLORKEY;
    return SetLayeredWindowAttributes (window, KEY_COLOUR, static_cast<BYTE> (s_style.alpha), flags) != FALSE;
}

bool IsOverlayWindow (HWND window)
{
    return window != nullptr && IsWindow (window) && HasOverlayClass (window);
}

void Shutdown ()
{
    DestroyAll ();
    s_geometry.clear ();
    s_annotationFrame.reset ();
    s_annotationLayerClaimed = false;
    if (s_classRegistered) {
        UnregisterClassW (OVERLAY_CLASS, OwnModule ());
        s_classRegistered = false;
    }
}

LONG PaintCount ()
{
    return s_paintCount;
}
DWORD LastPaintTick ()
{
    return s_lastPaintTick;
}

void SetGeometry (const std::vector<Polyline>& polylines)
{
    s_geometry = polylines;
    if (HWND h = Current ())
        InvalidateRect (h, nullptr, TRUE);
}

void SetAnnotationFrame (std::shared_ptr<const annotation::Frame> frame)
{
    s_annotationLayerClaimed = true;
    s_annotationFrame = std::move (frame);
    if (HWND h = Current ())
        InvalidateRect (h, nullptr, TRUE);
}

void SetTracking (bool enable, UINT intervalMs)
{
    HWND h = Current ();
    if (h == nullptr)
        return;

    if (enable) {
        if (intervalMs < 10)
            intervalMs = 10;
        s_requestedIntervalMs = intervalMs;
        const TransformObservation observation = DeriveTransform ();
        if (observation.affine.valid) {
            AdoptPlanTransform (observation.affine, s_acceptedTransform);
            s_transform = observation.reported;
        }
        s_stats.tracking = true;
        s_fastTimer = false;
        s_lastActivityTick = GetTickCount64 ();
        SetTimerCadence (h, true);
        InvalidateRect (h, nullptr, TRUE);
    }
    else {
        KillTimer (h, TRACK_TIMER_ID);
        s_stats.tracking = false;
    }
}

TrackStats GetTrackStats ()
{
    TrackStats s = s_stats;
    s.transform = s_transform;
    return s;
}

Transform ComputeTransform ()
{
    const TransformObservation observation = DeriveTransform ();
    if (observation.affine.valid) {
        AdoptPlanTransform (observation.affine, s_acceptedTransform);
        s_transform = observation.reported;
    }
    return s_transform;
}

std::vector<CalibRow> Calibrate ()
{
    std::vector<CalibRow> rows;

    HWND overlay = Current ();
    if (overlay == nullptr || s_canvas == nullptr)
        return rows;

    // The RAW affine, before any scaling correction: implied sizes have to be
    // compared in the projection's own units for the ratios to mean anything.
    RECT cr = {};
    GetClientRect (s_canvas, &cr);
    const short w = static_cast<short> (cr.right - cr.left);
    const short h = static_cast<short> (cr.bottom - cr.top);
    if (w < 8 || h < 8)
        return rows;

    API_Point pa = { static_cast<short> (h / 8), static_cast<short> (w / 8) };
    API_Point pb = { static_cast<short> (h - h / 8), static_cast<short> (w - w / 8) };
    API_Coord ca = {}, cb = {};
    API_Box zoom = {};
    if (ACAPI_View_PointToCoord (&pa, &ca) != NoError || ACAPI_View_PointToCoord (&pb, &cb) != NoError ||
        ACAPI_View_GetZoom (&zoom, nullptr) != NoError) {
        return rows;
    }

    const double mdx = cb.x - ca.x;
    const double mdy = cb.y - ca.y;
    if (std::fabs (mdx) < 1e-12 || std::fabs (mdy) < 1e-12)
        return rows;

    const double sx = (static_cast<double> (pb.h) - pa.h) / mdx;
    const double sy = (static_cast<double> (pb.v) - pa.v) / mdy;
    const double ox = pa.h - (ca.x * sx);
    const double oy = pa.v - (ca.y * sy);

    const double impliedW = std::fabs ((zoom.xMax - zoom.xMin) * sx);
    const double impliedH = std::fabs ((zoom.yMax - zoom.yMin) * sy);

    // Every window the projection could plausibly be expressed in. The one it IS
    // expressed in is the one whose two axes agree — a wrong window differs from
    // the right one by UNEQUAL insets, so its kx and ky come apart.
    struct Candidate {
        const char* label;
        HWND hwnd;
    };
    HWND doc = GetParent (overlay);
    HWND frame = doc != nullptr ? GetParent (doc) : nullptr;
    HWND top = frame != nullptr ? GetParent (frame) : nullptr;
    const Candidate candidates[] = {
        { "canvas", s_canvas }, { "document", doc }, { "mdiclient", frame }, { "frame", top }, { "overlay", overlay },
    };

    for (const Candidate& c : candidates) {
        if (c.hwnd == nullptr || !IsWindow (c.hwnd))
            continue;
        RECT r = {};
        GetClientRect (c.hwnd, &r);
        CalibRow row;
        row.label = c.label;
        row.clientW = static_cast<double> (r.right - r.left);
        row.clientH = static_cast<double> (r.bottom - r.top);
        row.impliedW = impliedW;
        row.impliedH = impliedH;
        row.kx = (impliedW > 1.0) ? row.clientW / impliedW : 0.0;
        row.ky = (impliedH > 1.0) ? row.clientH / impliedH : 0.0;
        row.disagree = (row.kx > 1e-9) ? std::fabs (row.kx - row.ky) / row.kx : 1.0;
        rows.push_back (row);
    }

    (void) ox;
    (void) oy;
    return rows;
}

} // namespace planoverlay
} // namespace geomsrv
