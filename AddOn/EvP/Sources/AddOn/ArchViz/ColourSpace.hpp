// ArchViz/ColourSpace — the sRGB transfer function, in both directions.
//
// ⚠️ WHY THIS EXISTS, AND IT IS NOT A STYLE PREFERENCE. Archicad's surface
// colours are DISPLAY-REFERRED (sRGB-encoded) and the renderer was multiplying
// them by light as if they were LINEAR reflectance. Measured, not assumed: the
// project's RAL paints carry exactly their published sRGB values.
//
//     surface              stored              RAL as sRGB         RAL as linear
//     Dažai - RAL 7016     0.220 0.243 0.259   0.220 0.243 0.259   0.040 0.048 0.054
//     Dažai - RAL 9010     0.945 0.925 0.882   0.945 0.925 0.882   0.880 0.839 0.753
//     Dažai - RAL 9016     0.945 0.941 0.918   0.945 0.941 0.918   0.880 0.871 0.823
//
// Three decimal places, three surfaces, exact against the sRGB column and
// nowhere near the linear one. (SurfaceTemplateDump, 2026-08-21.)
//
// ⚠️ THE ERROR IS LARGE AND IT IS NOT A UNIFORM BRIGHTNESS OFFSET, which is why
// no exposure slider could ever have hidden it. Anthracite grey was being used
// at 0.220 where it should be 0.040 -- five and a half times too much reflected
// light -- while white was 0.945 against 0.880, barely seven percent out. Dark
// materials were lifted enormously and light ones hardly at all, which flattens
// every contrast in the image and desaturates it. That is the "computer
// generated" look, and it poisons any material calibration done on top of it.
//
// ⚠️ THE OUTPUT SIDE WAS ALREADY RIGHT AND MUST NOT BE "FIXED" TOO. The render
// target is an _SRGB view, so the hardware applies the encode on write, and
// kArchVizMeshPSMain deliberately ends in linear. This file is only about the
// INPUT side: getting authored colour into the linear space the shading maths
// has always assumed. Adding an encode in the shader would gamma-correct twice.
//
// ⚠️ NOT EVERY 0..1 TRIPLE IS A COLOUR. The pick buffer carries element IDs
// packed into RGBA8 and must never go through either function -- see
// DiligentPickBuffer's "UNORM, NEVER _SRGB". Nor must a normal, a roughness, a
// depth or a motion vector. Convert colours; leave data alone.

#pragma once

namespace geomsrv {
namespace archviz {

// sRGB-encoded 0..1 -> linear 0..1. The exact piecewise IEC 61966-2-1 curve,
// not the pow(2.2) approximation: the approximation is wrong by up to 0.5% in
// the midtones and much more in the near-black segment, which is precisely
// where this project's dark facade colours live.
//
// Values outside 0..1 are clamped. ⚠️ A NEGATIVE INPUT IS NOT A COLOUR, and the
// odd-symmetry extension some libraries use would silently accept one.
float SrgbToLinear (float encoded);

// linear 0..1 -> sRGB-encoded 0..1. The exact inverse of the above.
//
// ⚠️ THE RENDERER DOES NOT NEED THIS ON ITS OUTPUT PATH -- the _SRGB render
// target view already encodes on write. It is here for the export path
// (RE51.B10), which writes files rather than render targets and therefore has
// no hardware doing it, and so that the round trip can be tested.
float LinearToSrgb (float linear);

} // namespace archviz
} // namespace geomsrv
