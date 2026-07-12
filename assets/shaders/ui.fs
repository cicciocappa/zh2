#version 330 core
// Screen-space UI for the game shell (menus, briefing, HUD text): UNLIT
// color, alpha smuggled in vNormal.x, font-atlas UV in vNormal.yz. Pairs
// with flat.vs so the 9-float vertex layout stays shared with the world
// geometry buffers. Text quads sample their glyph's coverage; solid quads
// point their UV at a reserved solid-white block in the atlas, so the whole
// UI stays one shader and one draw call (no per-fragment branch).
in vec3 vNormal;
in vec3 vColor;
uniform sampler2D uTex;
out vec4 FragColor;
void main() {
    FragColor = vec4(vColor, vNormal.x * texture(uTex, vNormal.yz).r);
}
