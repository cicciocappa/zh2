#version 330 core
// Sagoma-cadavere: albedo bakato (griglia colonna×outfit) illuminato per-pixel.
//   FLAT (uNormalLit=0)   = albedo con normale costante = su (shading uniforme).
//   NORMAL (uNormalLit=1) = albedo rilluminato con la normale WORLD bakata,
//                           ruotata per heading e illuminata dal sole NW
//                           WORLD-FIXED (= il sole della scena). Anti-piattume.
in vec2 vUV;        // albedo
in vec2 vUVn;       // normal (riga unica)
in float vHeading;
out vec4 FragColor;
uniform sampler2D uAlbedo;
uniform sampler2D uNormal;
uniform int uNormalLit;       // 0 = flat, 1 = relight per-pixel

// Sole NW identico a vat.fs / corpsebake.fs.
const vec3 LIGHT_DIR = normalize(vec3(-0.6, 0.9, -0.5));
const vec3 KEY  = vec3(1.0, 0.97, 0.90);
const vec3 AMB  = vec3(0.40, 0.42, 0.50);

void main() {
    vec4 a = texture(uAlbedo, vUV);
    if (a.a < 0.04) discard;              // fuori dalla sagoma
    vec3 n;
    if (uNormalLit == 1) {
        // normale bakata (heading 0) → world: stessa rot di vat.vs (vedi .vs).
        vec3 nb = texture(uNormal, vUVn).rgb * 2.0 - 1.0;
        float c = cos(vHeading), s = sin(vHeading);
        n = normalize(vec3(c*nb.x + s*nb.z, nb.y, -s*nb.x + c*nb.z));
    } else {
        n = vec3(0.0, 1.0, 0.0);
    }
    float ndl = max(dot(n, LIGHT_DIR), 0.0);
    vec3 col = a.rgb * (AMB + KEY * ndl);
    FragColor = vec4(col * 0.85, a.a);    // leggero scurimento "posato a terra"
}
