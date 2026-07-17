#version 330 core
// Skinned mesh, NW directional key + ambient (matches mesh.fs / flat.fs
// lighting so the soldier sits in the same light as the rest of the scene).
in vec3 vNormal;
in vec2 vUV;
uniform vec4 uBaseColor;
uniform int  uHasTexture;
uniform sampler2D uTexture;
out vec4 FragColor;
void main() {
    vec3 L = normalize(vec3(-0.5, 1.0, -0.6));
    float d = max(dot(normalize(vNormal), L), 0.0);
    float lit = 0.40 + 0.60 * d;
    vec4 base = uBaseColor;
    if (uHasTexture == 1) base *= texture(uTexture, vUV);
    FragColor = vec4(base.rgb * lit, base.a);
}
