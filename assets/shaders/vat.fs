#version 330 core
out vec4 FragColor;

in vec3 vNormal;
in vec2 vUV;
in vec3 vTint;

uniform sampler2D texDiff;
uniform int uHasTex;

// Chiave NW (GFX_DESIGN §4): luce direzionale da Nord-Ovest, in alto.
const vec3 LIGHT_DIR = normalize(vec3(-0.6, 0.9, -0.5));
const vec3 KEY  = vec3(1.0, 0.97, 0.90);
const vec3 AMB  = vec3(0.40, 0.42, 0.50);

void main() {
    vec3 n = normalize(vNormal);
    float ndl = max(dot(n, LIGHT_DIR), 0.0);
    vec3 base = (uHasTex == 1) ? texture(texDiff, vUV).rgb : vTint;
    vec3 col = base * (AMB + KEY * ndl);
    FragColor = vec4(col, 1.0);
}
