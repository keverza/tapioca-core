// ArchViz's uniforms, in ONE place so the shader side and Uniforms.hpp cannot
// drift. Packed as a vec4 array, following bgfx's 18-ibl example: bgfx uploads
// uniforms as vec4s anyway, and one setUniform of a small array beats several
// of scalars.
//
// ⚠️ THE INDEX OF EACH FIELD IS PART OF THE ABI between this file and
// ArchViz/Uniforms.hpp. Adding a field means appending, never inserting.

uniform vec4 u_archvizParams[3];

// xyz — direction TOWARD the sun, world space, Z-up, normalised.
//       Z-up because ArchViz converts nothing: Archicad is Z-up and metres, and
//       the camera is given an up vector of {0,0,1} instead (plan §6.4). The
//       axis swap that looks equivalent is a MIRROR and has shipped as a bug in
//       this repo once.
// w   — ambient term, 0..1. What a surface facing away from the sun still gets.
#define u_sunDir     u_archvizParams[0].xyz
#define u_ambient    u_archvizParams[0].w

// rgb — THE SURFACE'S OWN COLOUR, set once per DRAW RANGE, not per vertex.
//       A draw range is one material (ArchViz/MeshGroups.hpp), so the colour is
//       constant across it and a uniform is the honest place for it.
//       ⚠️ IT CANNOT BE A VERTEX ATTRIBUTE. VertexWeld merges corners that share
//       a position AND a normal, and two coplanar polygons with DIFFERENT
//       surfaces do exactly that — a wall face and the reveal beside it. Baking
//       the colour per vertex would make those shared corners take whichever
//       material was written last and bleed a gradient across the seam.
// a   — opacity, 1 = opaque. Fed from ModelerAPI's transparency, ALREADY
//       FLIPPED by the producer (SurfaceMaterial::alpha).
#define u_baseColor  u_archvizParams[1]

// xyz — the CAMERA's world position. Needed by the fragment shader to decide
//       which side of a face it is looking at, which is what makes two-sided
//       lighting possible without throwing the sun's direction away (see
//       fs_archviz_mesh.sc). w is spare.
#define u_eyePos     u_archvizParams[2].xyz
