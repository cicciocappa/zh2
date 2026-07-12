#version 330 core
// Screen-space UI images (title screen, menu button plates): RGBA texture
// modulated by tint (vColor) and alpha (vNormal.x), UV in vNormal.yz.
// Pairs with flat.vs like ui.fs, same 9-float vertex layout.
in vec3 vNormal;
in vec3 vColor;
uniform sampler2D uTex;
out vec4 FragColor;
void main() {
    vec4 t = texture(uTex, vNormal.yz);
    FragColor = vec4(t.rgb * vColor, t.a * vNormal.x);
}
