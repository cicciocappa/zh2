#version 330 core
// Billboard particles: a unit quad (centered, side 1) per-instance transformed
// by a camera-facing model matrix (right/up baked in fx_collect), tinted by a
// per-instance RGBA, optionally textured from an atlas tile (iSprite).
layout (location = 0) in vec3 aPos;     // quad vertex in [-0.5,0.5]
layout (location = 1) in mat4 iModel;   // locations 1..4
layout (location = 5) in vec4 iColor;
layout (location = 6) in float iSprite;
uniform mat4 uVP;
out vec2 vUV;        // 0..1 across the quad
out vec4 vColor;
out float vSprite;
void main() {
    gl_Position = uVP * iModel * vec4(aPos, 1.0);
    vUV = aPos.xy + 0.5;
    vColor = iColor;
    vSprite = iSprite;
}
