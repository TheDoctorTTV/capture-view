#version 450
// Stats/help/status panels: solid-colour triangles given in window pixels.
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 0) out vec4 v_color;

layout(push_constant) uniform Params {
  vec2 inverse_size;
} pc;

void main() {
  v_color = a_color;
  gl_Position = vec4(a_position * pc.inverse_size * 2.0 - 1.0, 0.0, 1.0);
}
