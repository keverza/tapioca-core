$input a_position
$output v_color0

// The PICKING pass's vertex shader: position only.
//
// ⚠️ IT SHARES varying.def.sc WITH THE MESH SHADERS, so `v_color0` is declared
// there and is emitted here only because shaderc wants at least one varying.
// Nothing reads it — the fragment shader writes a flat id colour from a uniform,
// because the id is a property of the ELEMENT (one draw call), not of a vertex.

#include <bgfx_shader.sh>

// ⚠️ `void main()`, NO SPACE — shaderc looks for that exact string.
void main()
{
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
    v_color0    = vec4(1.0, 1.0, 1.0, 1.0);
}
