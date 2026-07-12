#version 330 core
// Ring impostor sagoma-cadavere (CORPSE_DESIGN §10.8, esperimento fxlab).
// La camera di gioco ha elevazione FISSA e yaw libero: il dominio delle viste è
// un anello 1D di azimuth -> N viste bakate a elevazione RING_EL, riga k della
// griglia. Il quad è un billboard nel piano di vista (right/up2 della camera),
// ancorato a terra + uLookY (= il look-at del bake) e spinto verso la camera di
// un nudge costante (in ortho non sposta i pixel, solo la profondità: niente
// z-fight col terreno alla linea di contatto).
// Scelta vista: world = Ry(heading)·model => la direzione camera nel model frame
// ha azimuth (az_camera - heading). Vista k bakata con camera ad azimuth k·2π/N.
layout (location = 0) in vec2 aQuad;     // [-1,1] x [-1,1]
layout (location = 1) in vec4 iPosHd;    // xyz = ancora a terra (world), w = heading (rad)
layout (location = 2) in vec3 iCellOut;  // x = semi-lato (m), y = colonna (var*NPOSE+posa),
                                         // z = outfit (blocco di righe nell'anello ALBEDO; con
                                         //     uNOut=1 viene clampato a 0 — fxlab ribaka al cambio)
uniform mat4 uVP;
uniform vec3 uRight;      // right della camera (world, orizzontale)
uniform vec3 uUp2;        // up nel piano di vista (world)
uniform vec3 uToCam;      // verso la camera (unitario) per il nudge di profondità
uniform float uLookY;     // quota del centro-bake sopra l'ancora (m)
uniform float uCamAz;     // azimuth camera (rad, convenzione eye = ctr + (sin az, ., cos az))
uniform float uNCols;     // colonne della griglia (varianti*pose)
uniform float uNView;     // viste dell'anello (righe per blocco outfit)
uniform float uNOut;      // blocchi outfit nell'anello ALBEDO (1 = outfit singolo)
uniform int uBlendViews;  // 0 = snap alla vista più vicina, 1 = crossfade 2 viste

out vec2 vCellUV;         // uv dentro la cella [0,1]
out float vHeading;       // per ruotare la normale bakata a render-time
flat out float vCol;
flat out float vOut;      // blocco outfit (albedo; la normale è outfit-indipendente)
flat out float vRow0, vRow1, vFrac;

void main() {
    float hs = iCellOut.x;
    vec3 world = iPosHd.xyz + vec3(0.0, uLookY, 0.0) + uToCam * 0.6
               + uRight * (aQuad.x * hs) + uUp2 * (aQuad.y * hs);
    gl_Position = uVP * vec4(world, 1.0);
    vCellUV  = aQuad * 0.5 + 0.5;
    vHeading = iPosHd.w;
    vCol     = iCellOut.y;
    vOut     = clamp(iCellOut.z, 0.0, uNOut - 1.0);

    const float TWO_PI = 6.28318530718;
    float t = mod((uCamAz - iPosHd.w) / TWO_PI, 1.0) * uNView;   // vista frazionaria [0,N)
    float k0 = floor(t), fr = t - k0;
    if (uBlendViews == 0) { k0 = floor(t + 0.5); fr = 0.0; }
    if (k0 >= uNView) k0 = 0.0;
    vRow0 = k0;
    vRow1 = (k0 + 1.0 >= uNView) ? 0.0 : k0 + 1.0;
    vFrac = fr;
}
