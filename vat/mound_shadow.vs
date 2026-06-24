#version 330 core
// Contact shadow / AO ellittico sotto i mound di cadaveri (CORPSE_DESIGN §10.3):
// il disco unitario (riusa la geom delle ombre agente) è scalato sui semi-assi
// dell'ellisse del footprint e ruotato sull'asse principale → l'ombra segue una
// pila allungata invece di restare tonda. Falloff e colore in shadow.fs.
layout (location = 0) in vec2 aDisc;     // unit disc xy (radius 1)
layout (location = 1) in vec3 iCenter;   // xyz = ground center (world)
layout (location = 2) in vec4 iEll;      // ra, rb, cos, sin (semi-assi + rotazione)
uniform mat4 uVP;
out vec2 vD;
void main() {
    float px = aDisc.x * iEll.x, pz = aDisc.y * iEll.y;   // scala anisotropa
    float wx = px * iEll.z - pz * iEll.w;                 // rotazione asse principale
    float wz = px * iEll.w + pz * iEll.z;
    vec3 world = iCenter + vec3(wx, 0.0, wz);
    gl_Position = uVP * vec4(world, 1.0);
    vD = aDisc;
}
