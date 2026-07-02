#version 330 core
// Turret tracer streaks: per-vertex world position + RGBA. Drawn as GL_LINES
// with additive blend; the tail vertex carries alpha 0 and the head alpha ~1 so
// the interpolated segment fades into a comet trail. No lighting (it's a glow).
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec4 aColor;
uniform mat4 uVP;
out vec4 vColor;
void main() {
    gl_Position = uVP * vec4(aPos, 1.0);
    vColor = aColor;
}
