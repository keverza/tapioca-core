#ifndef EVP_ARCHVIZ_DXGI_OVERLAYSTYLE_HPP
#define EVP_ARCHVIZ_DXGI_OVERLAYSTYLE_HPP

// What an overlay does about depth, stated as policy rather than compiled in
// (PLAT-RE155, docs/architecture/api/HANDOFF-OverlayPatch.md stage 9).
//
// ⚠️ THREE OVERLAY KINDS WANT THREE DIFFERENT ANSWERS AND THE DIFFERENCE IS NOT
// A DETAIL. Solid ghost geometry must occlude itself; a wireframe must not, or
// its own far edges vanish behind its near ones and it stops being a wireframe;
// a heatmap lies ON a host surface and must not fight that surface for the same
// depth value. Hard-coding any one of those into the mesh renderer makes the
// other two impossible, which is why this is a parameter and not an `if`.
//
// ⚠️ AND HOST OCCLUSION IS A SEPARATE AXIS FROM SELF-OCCLUSION. Runs
// forty-eight to fifty-one were all confusion between them: the overlay tested
// against Archicad's raw depth buffer, which conflates "the wall is in front of
// you" with "a transparent build plane is in front of you" with "a helper
// gizmo drew here". One buffer cannot express a policy; two can.
//
//     HostOccluderDepth   what of the BUILDING should hide the overlay
//     OverlayDepth        what of the OVERLAY should hide the overlay
//
// Neither is Archicad's own depth-stencil view by default. See `HostOccluders`.

#include <cstdint>

namespace geomsrv {
namespace archviz {
namespace dxgi {
namespace overlay {

// ⚠️ WHERE THE HOST'S OCCLUDING DEPTH COMES FROM, AND `AsRendered` IS NOT THE
// DEFAULT ANY MORE. Run forty-eight measured 26% of a primitive IN FRONT of the
// model being rejected by Archicad's own buffer, and run fifty-one located the
// draw responsible: at close zoom the ghost lost 66% of its pixels after draw
// #2 while the BEHIND probe was still correctly rejected, so the model's depth
// was present and something else had been written over it.
//
// Archicad's buffer is the truth about Archicad's PIXELS. It is not the truth
// about what a designer means by "behind the wall".
enum class HostOcclusionMode : uint32_t {
    // Nothing of the host occludes the overlay. The control, and the only mode
    // in which a fault can be attributed to the overlay alone.
    None = 0,
    // ⚠️ SEMANTIC, AND THE PRODUCTION DEFAULT. Depth rendered by us from
    // extracted host surfaces that are OPAQUE by their Archicad material alpha.
    // Glass, build planes and helpers contribute nothing, so they cannot hide
    // the overlay -- which is the fault the user has reported since run
    // forty-eight and which no amount of better sampling of Archicad's buffer
    // could have fixed.
    OpaqueHostSurfaces = 1,
    // Archicad's own depth-stencil view, whatever is in it. Kept because it is
    // the only mode that matches Archicad's pixels exactly, and because a
    // comparison needs its control; never the default.
    AsRendered = 2,
};

// What an overlay of a given kind does. ⚠️ EVERY FIELD IS A DECISION SOMEBODY
// HAS TO MAKE, and the defaults are solid ghost geometry because that is the
// case already proven.
struct OverlayStyle {
    HostOcclusionMode hostOcclusion = HostOcclusionMode::OpaqueHostSurfaces;

    // ⚠️ SELF-OCCLUSION IS DEPTH WRITE PLUS DEPTH TEST AGAINST OUR OWN BUFFER,
    // and the two are separate because a wireframe wants the test and not the
    // write: its near edges should be hidden by solid ghost surfaces, and its
    // own far edges should not be hidden by its near ones.
    bool selfOcclusion = true;
    bool depthWrite = true;

    float opacity = 1.0f;

    // ⚠️ WHAT HAPPENS BEHIND AN OCCLUDER, AND ZERO IS A REAL CHOICE RATHER THAN
    // AN ABSENCE. At 0 the overlay is hidden, which is what solid ghost geometry
    // wants. Above 0 it is drawn faded, which is what an analysis wireframe
    // usually wants -- "this beam continues behind the wall" is information, and
    // hiding it throws that information away.
    float hiddenOpacity = 0.0f;

    // ⚠️ FOR GEOMETRY THAT LIES ON A HOST SURFACE. A heatmap painted on a wall
    // is coplanar with it; without a bias the two fight for the same depth value
    // and the result stipples. In NDC units, subtracted from the overlay's depth
    // so it wins.
    float depthBias = 0.0f;
};

// The three kinds this rung has to demonstrate together, each with the policy
// that makes it what it is.
inline OverlayStyle SolidGhostStyle ()
{
    OverlayStyle style;
    style.hostOcclusion = HostOcclusionMode::OpaqueHostSurfaces;
    style.selfOcclusion = true;
    style.depthWrite = true;
    style.opacity = 1.0f;
    style.hiddenOpacity = 0.0f;
    return style;
}

inline OverlayStyle WireframeStyle ()
{
    OverlayStyle style;
    style.hostOcclusion = HostOcclusionMode::OpaqueHostSurfaces;
    // ⚠️ TESTS BUT DOES NOT WRITE. A wireframe that wrote depth would occlude
    // itself edge by edge and read as a mesh with holes in it.
    style.selfOcclusion = true;
    style.depthWrite = false;
    style.opacity = 1.0f;
    // Behind a wall it fades rather than vanishing, because where a member goes
    // after it enters the wall is exactly what an analysis overlay is for.
    style.hiddenOpacity = 0.25f;
    return style;
}

inline OverlayStyle HeatmapStyle ()
{
    OverlayStyle style;
    style.hostOcclusion = HostOcclusionMode::OpaqueHostSurfaces;
    // It IS the host surface, so it must not be occluded by it, and it has
    // nothing of its own to occlude.
    style.selfOcclusion = false;
    style.depthWrite = false;
    style.opacity = 0.85f;
    style.hiddenOpacity = 0.0f;
    // ⚠️ PULLED TOWARDS THE CAMERA BY A HAIR. Coplanar with the surface it
    // describes; without this the two z-fight and the map stipples.
    style.depthBias = 0.0005f;
    return style;
}

} // namespace overlay
} // namespace dxgi
} // namespace archviz
} // namespace geomsrv

#endif
