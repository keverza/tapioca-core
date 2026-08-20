// ArchViz's vertex/varying contract. shaderc reads this to build the
// bgfx::VertexLayout it stamps into every compiled blob, so the attribute list
// here and ArchViz/ArchVizVertex.hpp must agree EXACTLY — a mismatch is not a
// compile error, it is geometry that renders as garbage.

vec3 v_normalWorld : NORMAL   = vec3(0.0, 0.0, 1.0);
// The fragment's own world position. Added for TWO-SIDED LIGHTING: deciding
// which way a face is really pointing needs the direction to the eye, and that
// is per-fragment. See fs_archviz_mesh.sc for what it replaced and why.
vec3 v_worldPos    : TEXCOORD0 = vec3(0.0, 0.0, 0.0);
vec4 v_color0      : COLOR0   = vec4(1.0, 1.0, 1.0, 1.0);

vec3 a_position    : POSITION;
vec3 a_normal      : NORMAL;
vec4 a_color0      : COLOR0;
