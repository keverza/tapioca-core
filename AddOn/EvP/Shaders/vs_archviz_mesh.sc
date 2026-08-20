$input a_position, a_normal, a_color0
$output v_normalWorld, v_color0, v_worldPos

// ArchViz's one mesh vertex shader. World-space positions in, because
// geomsrv::ExtractElement emits world-space metres and there is no per-element
// model matrix to undo that (plan §6.2's ⚠️ on SetTransform).

#include <bgfx_shader.sh>
#include "uniforms.sh"

// ⚠️ `void main()`, NO SPACE. shaderc looks for that exact string to find the
// entry point; "void main ()" gets "Shader entry point 'void main()' is not
// found". The repo's space-before-paren style loses to the tool here.
void main()
{
    gl_Position   = mul(u_modelViewProj, vec4(a_position, 1.0));
    // No normal matrix: the model matrix is identity (see above), so the world
    // normal IS the vertex normal. When a model matrix arrives, so must
    // u_model's inverse-transpose — a rotated normal is not a rotated position.
    v_normalWorld = a_normal;
    v_color0      = a_color0;
    // Already world space (see above), so this is a pass-through.
    v_worldPos    = a_position;
}
