#version 330 core
// Terrain ground mesh (.glb loaded via cgltf): textured + lit.
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
uniform mat4 uVP;
out vec3 vNormal;
out vec2 vUV;
void main() {
    gl_Position = uVP * vec4(aPos, 1.0);
    vNormal = aNormal;
    vUV = aUV;
}
