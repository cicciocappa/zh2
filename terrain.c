/* terrain.c — see terrain.h. Render-only heightmap (.zhm) load/save/sample. */
#include "terrain.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

void terrain_free(Terrain *t)
{
    if (!t) return;
    free(t->z);
    free(t->hole);
    memset(t, 0, sizeof(*t));
}

int terrain_load(const char *path, Terrain *t)
{
    if (!t) return -1;
    memset(t, 0, sizeof(*t));

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    char magic[4];
    uint32_t dims[2];
    float hdr[3];   /* origin_x, origin_y, px_per_m */
    if (fread(magic, 1, 4, fp) != 4) { fclose(fp); return -2; }
    int v2 = memcmp(magic, "ZHM2", 4) == 0;
    if (!v2 && memcmp(magic, "ZHM1", 4) != 0) { fclose(fp); return -2; }
    if (fread(dims, sizeof(uint32_t), 2, fp) != 2 ||
        fread(hdr, sizeof(float), 3, fp) != 3) {
        fclose(fp); return -2;
    }

    int w = (int)dims[0], h = (int)dims[1];
    if (w <= 0 || h <= 0 || (long long)w * h > (1LL << 28)) {  /* sanity cap */
        fclose(fp); return -2;
    }

    size_t n = (size_t)w * (size_t)h;
    float *z = malloc(n * sizeof(float));
    if (!z) { fclose(fp); return -3; }
    if (fread(z, sizeof(float), n, fp) != n) {
        free(z); fclose(fp); return -2;
    }
    uint8_t *hole = NULL;
    if (v2) {
        hole = malloc(n);
        if (!hole) { free(z); fclose(fp); return -3; }
        if (fread(hole, 1, n, fp) != n) { free(hole); free(z); fclose(fp); return -2; }
    }
    fclose(fp);

    t->w = w; t->h = h;
    t->origin_x = hdr[0]; t->origin_y = hdr[1];
    t->px_per_m = hdr[2];
    t->z = z;
    t->hole = hole;
    return 0;
}

int terrain_save(const char *path, const Terrain *t)
{
    if (!t || !t->z || t->w <= 0 || t->h <= 0) return -1;

    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    uint32_t dims[2] = { (uint32_t)t->w, (uint32_t)t->h };
    float hdr[3] = { t->origin_x, t->origin_y, t->px_per_m };
    size_t n = (size_t)t->w * (size_t)t->h;

    /* ZHM2 iff a hole mask is present, else ZHM1 (back-compatible) */
    int ok = fwrite(t->hole ? "ZHM2" : "ZHM1", 1, 4, fp) == 4 &&
             fwrite(dims, sizeof(uint32_t), 2, fp) == 2 &&
             fwrite(hdr, sizeof(float), 3, fp) == 3 &&
             fwrite(t->z, sizeof(float), n, fp) == n;
    if (ok && t->hole) ok = fwrite(t->hole, 1, n, fp) == n;
    fclose(fp);
    return ok ? 0 : -2;
}

float terrain_z(const Terrain *t, float x, float y)
{
    if (!t || !t->z || t->w <= 0 || t->h <= 0) return 0.0f;

    /* world -> sample coords */
    float fx = (x - t->origin_x) * t->px_per_m;
    float fy = (y - t->origin_y) * t->px_per_m;

    /* clamp into [0, w-1] x [0, h-1]: edge samples extend, never extrapolate */
    if (fx < 0.0f) fx = 0.0f;
    if (fy < 0.0f) fy = 0.0f;
    float fxmax = (float)(t->w - 1), fymax = (float)(t->h - 1);
    if (fx > fxmax) fx = fxmax;
    if (fy > fymax) fy = fymax;

    int i0 = (int)fx, j0 = (int)fy;
    int i1 = i0 + 1 < t->w ? i0 + 1 : i0;
    int j1 = j0 + 1 < t->h ? j0 + 1 : j0;
    float tx = fx - (float)i0, ty = fy - (float)j0;

    const float *z = t->z;
    int w = t->w;
    float z00 = z[j0 * w + i0], z10 = z[j0 * w + i1];
    float z01 = z[j1 * w + i0], z11 = z[j1 * w + i1];
    float a = z00 + (z10 - z00) * tx;   /* lerp along x, row j0 */
    float b = z01 + (z11 - z01) * tx;   /* lerp along x, row j1 */
    return a + (b - a) * ty;            /* lerp along y */
}

int terrain_hole(const Terrain *t, float x, float y)
{
    if (!t || !t->hole || t->w <= 0 || t->h <= 0) return 0;

    /* nearest sample (the mask is boolean; bilinear makes no sense) */
    int i = (int)((x - t->origin_x) * t->px_per_m + 0.5f);
    int j = (int)((y - t->origin_y) * t->px_per_m + 0.5f);
    if (i < 0) i = 0; else if (i >= t->w) i = t->w - 1;
    if (j < 0) j = 0; else if (j >= t->h) j = t->h - 1;
    return t->hole[(size_t)j * t->w + i] ? 1 : 0;
}

void terrain_each_hole_cell(const Terrain *t, float cell, int gw, int gh,
                            void (*cb)(void *, int, int), void *user)
{
    if (!t || !t->hole || !cb || cell <= 0.0f) return;
    for (int cy = 0; cy < gh; cy++)
        for (int cx = 0; cx < gw; cx++)
            if (terrain_hole(t, ((float)cx + 0.5f) * cell,
                                ((float)cy + 0.5f) * cell))
                cb(user, cx, cy);
}
