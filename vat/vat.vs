#version 330 core

// Mesh (per-vertex): ID vertice in aPos.x, UV per atlante outfit (futuro)
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aUV;

// Instance: layout pulito (niente hack mat4) — vedi migrazione_3d.md §0
layout (location = 2) in vec4 iPosHeading;   // xyz = world pos, w = heading (rad)
layout (location = 3) in vec4 iScaleFrame;    // x = scala, yzw = frameA, frameB, mix
layout (location = 4) in vec4 iOutfitTint;    // x = outfit index, yzw = tinta RGB

uniform mat4 uVP;
uniform sampler2D texPos;
uniform sampler2D texNorm;
uniform vec2 texSize;       // (texW, texH)
uniform float rowsPerFrame;

out vec3 vNormal;   // world space
out vec2 vUV;
out vec3 vTint;

const float ATLAS_NX = 4.0;  // colonne
const float ATLAS_NY = 8.0;  // righe: 0..15 outfit normali, 16..31 insanguinati (+16)

vec2 vatUV(float id, float frame) {
    float rowL = floor((id + 0.1) / texSize.x);
    float col  = mod(id, texSize.x);
    float rowG = frame * rowsPerFrame + rowL;
    return vec2((col + 0.5) / texSize.x, (rowG + 0.5) / texSize.y);
}

void main() {
    float id = aPos.x;
    float fA = iScaleFrame.y, fB = iScaleFrame.z, mixK = iScaleFrame.w;

    vec3 posA = texture(texPos, vatUV(id, fA)).rgb;
    vec3 posB = texture(texPos, vatUV(id, fB)).rgb;
    vec3 local = mix(posA, posB, mixK);

    vec3 nA = texture(texNorm, vatUV(id, fA)).rgb;
    vec3 nB = texture(texNorm, vatUV(id, fB)).rgb;
    vec3 nLocal = normalize(mix(nA, nB, mixK));

    // Rotazione attorno a Y (heading) + scala + traslazione
    float h = iPosHeading.w;
    float c = cos(h), s = sin(h);
    mat3 rot = mat3( c, 0.0, -s,
                     0.0, 1.0, 0.0,
                     s, 0.0,  c );
    vec3 world = rot * (local * iScaleFrame.x) + iPosHeading.xyz;

    gl_Position = uVP * vec4(world, 1.0);
    vNormal = rot * nLocal;

    // Atlante outfit: UV modello / N + offset cella. 0 = alto-sx, ordine di lettura.
    // (1-v): Blender ha origine UV in basso-sx, la texture NON è flippata al load.
    float outfit = iOutfitTint.x;
    float col = mod(outfit, ATLAS_NX);
    float row = floor(outfit / ATLAS_NX);
    vUV = (vec2(aUV.x, 1.0 - aUV.y) + vec2(col, row)) / vec2(ATLAS_NX, ATLAS_NY);
    vTint = iOutfitTint.yzw;
}
