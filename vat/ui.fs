#version 330 core
// Screen-space UI for the game shell (menus, briefing, HUD text): UNLIT
// color, alpha smuggled in vNormal.x. Pairs with flat.vs so the 9-float
// vertex layout stays shared with the world geometry buffers.
in vec3 vNormal;
in vec3 vColor;
out vec4 FragColor;
void main() {
    FragColor = vec4(vColor, vNormal.x);
}
