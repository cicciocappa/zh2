/* edit_pick.h — screen->world picking for the level editor (EDITOR_DESIGN §3).
 *
 * Header-only, ZERO deps (operates on raw float[16] column-major matrices, the
 * same layout vat_gl.h's mat4 uses). Included by both vat_horde (the host) and
 * test_pick.c (headless math unit test, design §6 verification (b)).
 *
 * The editor needs to turn a mouse pixel into a world point on the ground plane
 * y=0. We unproject the pixel to a near and a far clip-space point through
 * inverse(VP), then intersect that line with y=0. Works for ortho AND
 * perspective VP (the editor camera is ortho, but the math is general).
 */
#ifndef EDIT_PICK_H
#define EDIT_PICK_H

/* General 4x4 inverse (cofactor expansion, MESA gluInvertMatrix). Column-major
 * in == column-major out. Returns 1 on success, 0 if singular. */
static int m4_invert(const float m[16], float out[16]) {
    float inv[16], det;
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15]
             + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15]
             - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15]
             + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14]
             - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15]
             - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15]
             + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15]
             - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14]
             + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15]
             + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15]
             - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15]
             + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14]
             - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11]
             - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11]
             + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11]
             - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10]
             + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

    det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det > -1e-20f && det < 1e-20f) return 0;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) out[i] = inv[i] * det;
    return 1;
}

/* column-major 4x4 * vec4 -> vec4 */
static void m4_mulv(const float m[16], const float v[4], float out[4]) {
    for (int r = 0; r < 4; r++)
        out[r] = m[0*4+r]*v[0] + m[1*4+r]*v[1] + m[2*4+r]*v[2] + m[3*4+r]*v[3];
}

/* Unproject pixel (mx,my, top-left origin) in a W x H viewport to the world
 * segment near->far. For callers that intersect several horizontal planes
 * (hover over tall structures) without re-inverting VP per plane. */
static int pick_ray(const float vp[16], float mx, float my, int W, int H,
                    float p0out[3], float p1out[3]) {
    float inv[16];
    if (!m4_invert(vp, inv)) return 0;
    float ndc_x = 2.0f*mx/(float)W - 1.0f;
    float ndc_y = 1.0f - 2.0f*my/(float)H;          /* flip: pixels go down */
    float nearc[4] = { ndc_x, ndc_y, -1.0f, 1.0f }; /* GL clip near */
    float farc [4] = { ndc_x, ndc_y,  1.0f, 1.0f }; /* GL clip far  */
    float p0[4], p1[4];
    m4_mulv(inv, nearc, p0);
    m4_mulv(inv, farc,  p1);
    if (p0[3] == 0.0f || p1[3] == 0.0f) return 0;
    for (int i = 0; i < 3; i++) { p0out[i] = p0[i]/p0[3]; p1out[i] = p1[i]/p1[3]; }
    return 1;
}

/* Intersect the unprojected segment with the horizontal plane y=h. Returns 1
 * and writes world (*wx,*wy) [the (x,z) in meters], 0 if parallel. */
static int pick_ray_plane(const float p0[3], const float p1[3], float h,
                          float *wx, float *wy) {
    float dy = p1[1] - p0[1];
    if (dy > -1e-9f && dy < 1e-9f) return 0;         /* ray parallel to plane */
    float t = (h - p0[1]) / dy;
    *wx = p0[0] + t*(p1[0] - p0[0]);
    *wy = p0[2] + t*(p1[2] - p0[2]);
    return 1;
}

/* Pixel onto the ground plane y=0 (the editor's original entry point). */
static int pick_y0(const float vp[16], float mx, float my, int W, int H,
                   float *wx, float *wy) {
    float p0[3], p1[3];
    if (!pick_ray(vp, mx, my, W, H, p0, p1)) return 0;
    return pick_ray_plane(p0, p1, 0.0f, wx, wy);
}

#endif /* EDIT_PICK_H */
