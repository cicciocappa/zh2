#version 330 core
// Terrain ground: base-color texture (or fallback color) * NW directional key.
in vec3 vNormal;
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform int  uHasTex;
uniform vec3 uColor;   // fallback when untextured
void main() {
    vec3 base = (uHasTex != 0) ? texture(uTex, vUV).rgb : uColor;
    vec3 L = normalize(vec3(-0.5, 1.0, -0.6));   // NW key, from above (matches flat.fs)
    float d = max(dot(normalize(vNormal), L), 0.0);
    float lit = 0.40 + 0.60 * d;
    FragColor = vec4(base * lit, 1.0);
}
