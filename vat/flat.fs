#version 330 core
// Flat-shaded static geometry: NW directional key + ambient (GFX_DESIGN §4).
in vec3 vNormal;
in vec3 vColor;
out vec4 FragColor;
void main() {
    vec3 L = normalize(vec3(-0.5, 1.0, -0.6));   // NW key, from above
    float d = max(dot(normalize(vNormal), L), 0.0);
    float lit = 0.40 + 0.60 * d;
    FragColor = vec4(vColor * lit, 1.0);
}
