#version 330 core
// Sagoma-cadavere a terra: quad unitario [-1,1]^2 orientato per heading e posato
// sul terreno, campiona l'atlante di sprite top-down bakati dai modelli VAT in
// posa di morte (una cella per variante, in riga). Persistente, blended.
// CORPSE_DESIGN §10.2 (tier vicino, sagoma del corpo) + richiesta utente.
layout (location = 0) in vec2 aQuad;     // [-1,1] x [-1,1]
layout (location = 1) in vec4 iPosHd;    // xyz = centro a terra (world), w = heading (rad)
layout (location = 2) in vec2 iSizeCell; // x = semi-lato (m), y = indice cella atlante
uniform mat4 uVP;
uniform float uNCells;                    // numero di celle (varianti) nell'atlante
out vec2 vUV;
void main() {
    float c = cos(iPosHd.w), s = sin(iPosHd.w);
    vec2 r = vec2(aQuad.x*c - aQuad.y*s, aQuad.x*s + aQuad.y*c) * iSizeCell.x;
    vec3 world = iPosHd.xyz + vec3(r.x, 0.0, r.y);
    gl_Position = uVP * vec4(world, 1.0);
    vec2 uv = aQuad*0.5 + 0.5;            // [0,1] dentro la cella
    vUV = vec2((uv.x + iSizeCell.y)/uNCells, uv.y);
}
