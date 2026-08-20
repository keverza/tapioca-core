$input v_color0

// The PICKING pass: every fragment of an element writes that element's ID as a
// colour, and the CPU reads the pixel back.
//
// ⚠️ NO LIGHTING, NO BLENDING, NO ANTIALIASING ANYWHERE IN THIS PASS. An id is
// not a colour to be interpolated — a blended edge between element 3 and element
// 260 reads back as element 131, which exists, is plausible and is wrong. The
// render state that submits this must keep MSAA and blending off for the same
// reason.

#include <bgfx_shader.sh>
#include "uniforms.sh"

// ⚠️ `void main()`, NO SPACE — shaderc looks for that exact string.
void main()
{
    // u_baseColor carries the id, already unpacked to 0..1 floats by the CPU —
    // see SceneCache::DrawIds. Alpha is 1 so a hit is distinguishable from the
    // cleared background, which is what "no element here" looks like.
    gl_FragColor = vec4(u_baseColor.rgb, 1.0);
}
