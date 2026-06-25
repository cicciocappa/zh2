#version 330 core
// Bake della sagoma-cadavere top-down, condivide vat.vs. Due modi (uMode):
//   0 = ALBEDO  (diffuse NON illuminato, bakato per (colonna × outfit) in griglia)
//   1 = NORMAL  (normale WORLD a heading 0, encoded *0.5+0.5; outfit-indipendente)
// L'alpha = copertura del corpo (clear a 0 → cutout). Il relighting per-pixel col
// sole NW world-fixed avviene in corpse_decal.fs (esperimento anti-piattume).
out vec4 FragColor;

in vec3 vNormal;   // world space (heading 0 in fase di bake)
in vec2 vUV;
in vec3 vTint;

uniform sampler2D texDiff;
uniform int uHasTex;
uniform int uMode;     // 0 = albedo, 1 = normal

void main() {
    if (uMode == 1) {
        FragColor = vec4(normalize(vNormal) * 0.5 + 0.5, 1.0);
    } else {
        vec3 base = (uHasTex == 1) ? texture(texDiff, vUV).rgb : vTint;
        FragColor = vec4(base, 1.0);
    }
}
