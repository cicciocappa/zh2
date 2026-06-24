#version 330 core
// Sagoma-cadavere: campiona l'atlante bakato (RGBA, alpha = copertura del corpo).
// Lo shading è già bakato nello sprite (chiave NW); qui solo cutout + leggero
// scurimento per "posarlo" nel terreno (corpo secco/freddo).
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uAtlas;
void main() {
    vec4 t = texture(uAtlas, vUV);
    if (t.a < 0.04) discard;              // fuori dalla sagoma
    FragColor = vec4(t.rgb * 0.85, t.a);
}
