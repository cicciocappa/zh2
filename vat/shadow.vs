#version 330 core
// Ground shadow blobs: a unit disc, instanced under each agent at terrain quota.
layout (location = 0) in vec2 aDisc;     // unit disc xy (radius 1)
layout (location = 1) in vec4 iCenterR;  // xyz = ground center (world), w = radius
uniform mat4 uVP;
out vec2 vD;
void main() {
    vec3 world = iCenterR.xyz + vec3(aDisc.x * iCenterR.w, 0.0, aDisc.y * iCenterR.w);
    gl_Position = uVP * vec4(world, 1.0);
    vD = aDisc;
}
