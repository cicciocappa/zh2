#version 330 core
// Particle fragment. Without an atlas (uHasAtlas=0, the current FX_LAB step):
// a soft round blob -- alpha falls off radially from the quad center, like the
// blood decal. With an atlas: sample the tile selected by vSprite over a
// uAtlasGrid x uAtlasGrid grid. Color/alpha multiplied by the per-instance tint.
in vec2 vUV;
in vec4 vColor;
in float vSprite;
uniform sampler2D uAtlas;
uniform float uAtlasGrid;
uniform int uHasAtlas;
out vec4 FragColor;
void main() {
    if (uHasAtlas == 1) {
        float g = uAtlasGrid;
        float idx = floor(vSprite + 0.5);
        vec2 tile = vec2(mod(idx, g), floor(idx / g));
        vec2 uv = (tile + vUV) / g;
        FragColor = texture(uAtlas, uv) * vColor;
    } else {
        // procedural soft disc
        float d = length(vUV - 0.5) * 2.0;            // 0 center .. 1 edge
        float a = vColor.a * (1.0 - smoothstep(0.35, 1.0, d));
        FragColor = vec4(vColor.rgb, a);
    }
}
