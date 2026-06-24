#version 330 core
// Static textured mesh (gore mesh-gibs: severed arm + chunks). Per-vertex
// pos/normal/uv, per-draw model matrix; UVs map into the zombie body diffuse.
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
uniform mat4 uVP;
uniform mat4 uModel;
out vec3 vNormal;
out vec2 vUV;
void main() {
    gl_Position = uVP * uModel * vec4(aPos, 1.0);
    vNormal = mat3(uModel) * aNormal;   // includes uniform scale (renormalized in fs)
    vUV = aUV;
}
