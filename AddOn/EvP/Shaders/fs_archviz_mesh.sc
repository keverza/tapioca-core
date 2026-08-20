$input v_normalWorld, v_color0, v_worldPos

// Lambert with an ambient floor, lit by ARCHICAD'S OWN sun vector.
//
// Deliberately not a PBR model. Phase 9 brings shadow maps, sky and AO from the
// absorbed examples; until then the job is only to make a solid read as a solid,
// and a flat-shaded box must look like a box rather than a silhouette.

#include <bgfx_shader.sh>
#include "uniforms.sh"

// ⚠️ `void main()`, NO SPACE. shaderc looks for that exact string to find the
// entry point; "void main ()" gets "Shader entry point 'void main()' is not
// found". The repo's space-before-paren style loses to the tool here.
void main()
{
    vec3  n = normalize(v_normalWorld);

    // ⚠️ TWO-SIDED, BUT NOT abs(dot(n, l)) — THAT WAS THE BUG THE 2026-08-07 RUN
    // FOUND. abs() lights a north-facing wall exactly as brightly as a
    // south-facing one, so the sun has no direction at all in the horizontal
    // plane: the building reads FLAT and evenly lit, and its lit side does not
    // agree with Archicad's. The reported symptom was read as "the sun never
    // arrived"; the sun was arriving and being thrown away here.
    //
    // The correct two-sided rule flips the normal toward the VIEWER — which is
    // what "we are looking at the back of this face" means — and then lights it
    // normally. Inward-facing polygons still shade instead of going black (the
    // reason abs() was there), and the sun keeps its direction.
    vec3  v = normalize(u_eyePos - v_worldPos);
    if (dot(n, v) < 0.0)
        n = -n;
    float ndotl = max(dot(n, u_sunDir), 0.0);
    float lit   = u_ambient + (1.0 - u_ambient) * ndotl;
    // TWO COLOURS, MULTIPLIED, and both are needed. u_baseColor is the SURFACE
    // (one per draw range, from Archicad's material pool); v_color0 is the
    // vertex colour, which real geometry leaves white and the debug cube uses to
    // paint its six faces differently. Multiplying means neither path needs its
    // own shader, and a scene that renders white is then diagnosable: white with
    // shading = the material table never arrived, black = the uniform was never
    // set at all.
    vec4  base  = v_color0 * u_baseColor;
    gl_FragColor = vec4(base.rgb * lit, base.a);
}
