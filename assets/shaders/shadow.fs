#version 330 core
// Soft dark blob: alpha falls off toward the disc edge.
in vec2 vD;
out vec4 FragColor;
void main() {
    float a = 0.40 * (1.0 - smoothstep(0.55, 1.0, length(vD)));
    FragColor = vec4(0.0, 0.0, 0.0, a);
}
