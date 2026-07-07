#version 330 core

// Mesh (per-vertex): ID vertice in aPos.x, UV per atlante outfit (futuro)
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aUV;

// Instance: layout pulito (niente hack mat4) — vedi migrazione_3d.md §0
layout (location = 2) in vec4 iPosHeading;   // xyz = world pos, w = heading (rad)
layout (location = 3) in vec4 iScaleFrame;    // x = scala, yzw = frameA, frameB, mix
layout (location = 4) in vec4 iOutfitTint;    // x = outfit index, yzw = tinta RGB
layout (location = 5) in vec2 iTumble;        // volo: pitch (X), roll (Z) — 0 a terra

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

    // Rotazione: yaw attorno a Y (heading) + tumble di volo (pitch su X, roll su Z)
    // attorno al mezzo-busto, così un corpo lanciato ruota su piu' assi. A terra
    // iTumble = 0 -> Rx = Rz = identita' -> identico al comportamento base.
    float h = iPosHeading.w;
    float cy = cos(h), sy = sin(h);
    mat3 Ry = mat3( cy, 0.0, -sy,   0.0, 1.0, 0.0,   sy, 0.0, cy );
    float cp = cos(iTumble.x), sp = sin(iTumble.x);
    mat3 Rx = mat3( 1.0, 0.0, 0.0,  0.0, cp, sp,   0.0, -sp, cp );   // pitch (X)
    float cr = cos(iTumble.y), sr = sin(iTumble.y);
    mat3 Rz = mat3( cr, sr, 0.0,   -sr, cr, 0.0,   0.0, 0.0, 1.0 );  // roll (Z)
    mat3 tumble = Rx * Rz;
    vec3 pivot = vec3(0.0, 0.9, 0.0);            // ~mezzo busto (model space, m)
    vec3 lt = tumble * (local - pivot) + pivot;  // tumble attorno al centro corpo
    vec3 world = Ry * (lt * iScaleFrame.x) + iPosHeading.xyz;

    gl_Position = uVP * vec4(world, 1.0);
    vNormal = Ry * tumble * nLocal;

    // Atlante outfit: UV modello / N + offset cella. 0 = alto-sx, ordine di lettura.
    // (1-v): Blender ha origine UV in basso-sx, la texture NON è flippata al load.
    float outfit = iOutfitTint.x;
    float col = mod(outfit, ATLAS_NX);
    float row = floor(outfit / ATLAS_NX);
    vUV = (vec2(aUV.x, 1.0 - aUV.y) + vec2(col, row)) / vec2(ATLAS_NX, ATLAS_NY);
    vTint = iOutfitTint.yzw;
}
