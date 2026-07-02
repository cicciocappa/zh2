#version 330 core
// Sagoma-cadavere a terra: quad unitario [-1,1]^2 orientato per heading e posato
// sul terreno, campiona l'atlante di sprite top-down bakati dai modelli VAT in
// posa di morte. Persistente, blended. CORPSE_DESIGN §10.2 + richiesta utente.
// L'atlante ALBEDO è una griglia 2D (colonna = variante*NPOSE+posa, riga = outfit);
// la NORMAL map è una sola riga (outfit-indipendente) → due UV distinte.
layout (location = 0) in vec2 aQuad;     // [-1,1] x [-1,1]
layout (location = 1) in vec4 iPosHd;    // xyz = centro a terra (world), w = heading (rad)
layout (location = 2) in vec3 iCellOut;  // x = semi-lato (m), y = colonna, z = outfit (riga)
uniform mat4 uVP;
uniform float uNCols;                     // colonne dell'atlante (varianti*pose)
uniform float uNOutfit;                   // righe dell'atlante albedo (outfit)
out vec2 vUV;       // albedo (griglia colonna×outfit)
out vec2 vUVn;      // normal (una riga)
out float vHeading; // per ruotare la normale a render-time
void main() {
    // STESSA rotazione di vat.vs (rot attorno a Y): footprint (X,Z) -> world
    // (c*X + s*Z, -s*X + c*Z). Il quad-x mappa model-X, quad-y model-Z (vedi U-flip
    // sotto). Il segno opposto ruoterebbe la sagoma di 2*heading vs la mesh.
    float c = cos(iPosHd.w), s = sin(iPosHd.w);
    vec2 r = vec2(aQuad.x*c + aQuad.y*s, -aQuad.x*s + aQuad.y*c) * iCellOut.x;
    vec3 world = iPosHd.xyz + vec3(r.x, 0.0, r.y);
    gl_Position = uVP * vec4(world, 1.0);
    vec2 uv = aQuad*0.5 + 0.5;            // [0,1] dentro la cella
    uv.x = 1.0 - uv.x;                    // la bake (occhio +Y, up +Z) specchia X
    float u = (uv.x + iCellOut.y) / uNCols;
    vUV  = vec2(u, (uv.y + iCellOut.z) / uNOutfit);
    vUVn = vec2(u, uv.y);
    vHeading = iPosHd.w;
}
