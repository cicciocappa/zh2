#version 330 core
// Blood decals (GFX §5.2): a unit disc instanced on the ground, per-instance
// center+radius and color. Persistent stains where bodies fell / exploded.
layout (location = 0) in vec2 aDisc;     // unit disc xy (radius 1)
layout (location = 1) in vec4 iCenterR;  // xyz = ground center (world), w = radius
layout (location = 2) in vec3 iColor;    // blood tint
uniform mat4 uVP;
out vec2 vD;
out vec3 vCol;
void main() {
    vec3 world = iCenterR.xyz + vec3(aDisc.x * iCenterR.w, 0.0, aDisc.y * iCenterR.w);
    gl_Position = uVP * vec4(world, 1.0);
    vD = aDisc;
    vCol = iColor;
}
