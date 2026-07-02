#version 330 core
// Textured static mesh, NW directional key + ambient (matches flat.fs lighting).
in vec3 vNormal;
in vec2 vUV;
uniform sampler2D uTex;
out vec4 FragColor;
void main() {
    vec3 L = normalize(vec3(-0.5, 1.0, -0.6));
    float d = max(dot(normalize(vNormal), L), 0.0);
    float lit = 0.40 + 0.60 * d;
    vec4 t = texture(uTex, vUV);
    FragColor = vec4(t.rgb * lit, 1.0);
}
