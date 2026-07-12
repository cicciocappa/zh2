#version 330 core
// Ring impostor sagoma-cadavere: albedo per-vista + normale WORLD (heading 0)
// per-vista, relight col sole NW world-fixed dopo rotazione per heading (stessa
// logica di corpse_decal.fs modo NORMAL — il lit bakato a heading 0 sarebbe
// sbagliato per un cadavere ruotato: il sole gli girerebbe attorno).
in vec2 vCellUV;
in float vHeading;
flat in float vCol;
flat in float vOut;
flat in float vRow0, vRow1, vFrac;
out vec4 FragColor;

uniform sampler2D uAlbedo;   // griglia colonne × (uNOut blocchi × NVIEW righe)
uniform sampler2D uNormal;   // colonne × NVIEW righe, normale world a heading 0 (outfit-indip.)
uniform float uNCols, uNView, uNOut;

// Sole NW identico a vat.fs / corpse_decal.fs.
const vec3 LIGHT_DIR = normalize(vec3(-0.6, 0.9, -0.5));
const vec3 KEY  = vec3(1.0, 0.97, 0.90);
const vec3 AMB  = vec3(0.40, 0.42, 0.50);

vec2 albUV(float row) {
    return vec2((vCellUV.x + vCol) / uNCols,
                (vCellUV.y + vOut * uNView + row) / (uNOut * uNView));
}
vec2 nrmUV(float row) {
    return vec2((vCellUV.x + vCol) / uNCols, (vCellUV.y + row) / uNView);
}

void main() {
    vec4 a0 = texture(uAlbedo, albUV(vRow0));
    vec4 a1 = texture(uAlbedo, albUV(vRow1));
    vec4 a  = mix(a0, a1, vFrac);
    if (a.a < 0.15) discard;              // cutout (soglia > decal: taglia l'alone del crossfade)
    vec3 nb0 = texture(uNormal, nrmUV(vRow0)).rgb * 2.0 - 1.0;
    vec3 nb1 = texture(uNormal, nrmUV(vRow1)).rgb * 2.0 - 1.0;
    vec3 nb  = normalize(mix(nb0, nb1, vFrac));
    float c = cos(vHeading), s = sin(vHeading);
    vec3 n = normalize(vec3(c*nb.x + s*nb.z, nb.y, -s*nb.x + c*nb.z));
    float ndl = max(dot(n, LIGHT_DIR), 0.0);
    vec3 col = (a.rgb / max(a.a, 0.001)) * (AMB + KEY * ndl);   // un-premultiply del bordo bilineare
    FragColor = vec4(col * 0.85, a.a);    // scurimento "posato a terra" come il decal
}
