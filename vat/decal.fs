#version 330 core
// Soft blood stain: opaque-ish center, alpha falls off toward the edge.
in vec2 vD;
in vec3 vCol;
out vec4 FragColor;
void main() {
    float a = 0.80 * (1.0 - smoothstep(0.25, 1.0, length(vD)));
    FragColor = vec4(vCol, a);
}
