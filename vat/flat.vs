#version 330 core
// Static geometry (terrain obstacles, ground): per-vertex pos/normal/color.
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec3 aColor;
uniform mat4 uVP;
out vec3 vNormal;
out vec3 vColor;
void main() {
    gl_Position = uVP * vec4(aPos, 1.0);
    vNormal = aNormal;
    vColor = aColor;
}
