/* model_view — standalone skinned-glb previewer (soldier pipeline, model.c).
 *
 * Plays the animation clips of a skinned glTF/glb (Sketchfab model + Mixamo
 * clips) so you can judge model and animations before wiring them into
 * vat_horde. Same role sprite_view has for .zspr sheets.
 *
 *   ./model_view assets/models/soldier.glb
 *
 * Controls:
 *   Tab          next clip (cycles; clip list printed at startup)
 *   Space        play / pause
 *   Left Right   orbit camera
 *   Up Down      raise / lower camera
 *   - = (or +)   camera distance
 *   G            toggle ground grid
 *   R            reset camera + clip 0
 *   Esc / Q      quit
 *
 * Headless filmstrip (loader correctness check without eyeballing):
 *   MODEL_VIEW_SHOT="clip_idx" ./model_view file.glb
 *     renders 6 poses of the clip evenly spaced across its duration side by
 *     side to model_view_shot.bmp, then exits (0 = load + render OK).
 */
#define _POSIX_C_SOURCE 199309L
#include <SDL3/SDL.h>
#include <glad/glad.h>
#include "vat_gl.h"
#include "model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SW 960
#define SH 720

/* ground grid: flat-shader triangle soup would be overkill — immediate line
 * VBO with the skinned shader in unskinned mode (uBaseColor drives color). */
static GLuint grid_vao, grid_vbo;
static int grid_verts;
static void grid_build(void) {
    /* 21x21 lines on XZ, 1 m spacing, thin quads not needed: GL_LINES */
    static float v[21 * 2 * 2 * 16]; /* pos3+nrm3+uv2+joints4+weights4 = 16 floats */
    int n = 0;
    for (int i = -10; i <= 10; i++) {
        float a[2][3] = {{(float)i, 0, -10}, {(float)i, 0, 10}};
        float b[2][3] = {{-10, 0, (float)i}, {10, 0, (float)i}};
        for (int k = 0; k < 2; k++) {
            float *p = v + (size_t)n * 16;
            memcpy(p, a[k], 12); p[4] = 1; n++;
        }
        for (int k = 0; k < 2; k++) {
            float *p = v + (size_t)n * 16;
            memcpy(p, b[k], 12); p[4] = 1; n++;
        }
    }
    grid_verts = n;
    glGenVertexArrays(1, &grid_vao);
    glGenBuffers(1, &grid_vbo);
    glBindVertexArray(grid_vao);
    glBindBuffer(GL_ARRAY_BUFFER, grid_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n * 16 * sizeof(float), v, GL_STATIC_DRAW);
    GLsizei st = 16 * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, st, (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

static void grid_draw(GLuint prog, const mat4 vp) {
    mat4 id; m_identity(id);
    glUseProgram(prog);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uVP"), 1, GL_FALSE, vp);
    glUniformMatrix4fv(glGetUniformLocation(prog, "uModel"), 1, GL_FALSE, id);
    glUniform1i(glGetUniformLocation(prog, "uSkinned"), 0);
    glUniform1i(glGetUniformLocation(prog, "uHasTexture"), 0);
    glUniform4f(glGetUniformLocation(prog, "uBaseColor"), 0.28f, 0.30f, 0.34f, 1);
    glBindVertexArray(grid_vao);
    glDrawArrays(GL_LINES, 0, grid_verts);
    glBindVertexArray(0);
}

static void cam_vp(mat4 vp, float yaw, float height, float dist, float cy,
                   float aspect) {
    mat4 proj, view;
    float eye[3] = {sinf(yaw) * dist, height, cosf(yaw) * dist};
    float ctr[3] = {0, cy, 0}, up[3] = {0, 1, 0};
    m_persp(proj, 50.0f * 3.14159265f / 180.0f, aspect, 0.05f, 200.0f);
    m_lookat(view, eye, ctr, up);
    m_mul(vp, proj, view);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s model.glb\n", argv[0]);
        return 1;
    }
    const char *shot = getenv("MODEL_VIEW_SHOT");

    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_Window *win = SDL_CreateWindow("model_view", SW, SH,
                                       SDL_WINDOW_OPENGL | (shot ? SDL_WINDOW_HIDDEN : 0));
    if (!win) { fprintf(stderr, "SDL win: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx || !gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        fprintf(stderr, "GL context/glad failed\n"); return 1;
    }
    SDL_GL_SetSwapInterval(1);

    GLuint prog = vg_shader("assets/shaders/skinned.vs", "assets/shaders/skinned.fs");
    if (!prog) return 1;
    grid_build();

    Model mdl;
    if (!model_load(&mdl, argv[1])) return 1;
    for (int i = 0; i < mdl.clip_count; i++)
        printf("  clip %d: %-24s %.2fs\n", i, mdl.clips[i].name, mdl.clips[i].duration);

    AnimState anim;
    anim_state_init(&anim);
    if (mdl.clip_count > 0) anim_state_play(&anim, 0, true);

    /* auto-fit like vat_horde: bind-pose bbox scaled to 1.80 m, feet at the
     * origin, centered in XZ (the raw data may be at any unit scale) */
    mat4 fit; m_identity(fit);
    {
        float bh = mdl.bbox_max[1] - mdl.bbox_min[1];
        float sc = bh > 1e-3f ? 1.80f / bh : 1.0f;
        fit[0] = fit[5] = fit[10] = sc;
        fit[12] = -0.5f * (mdl.bbox_min[0] + mdl.bbox_max[0]) * sc;
        fit[13] = -mdl.bbox_min[1] * sc;
        fit[14] = -0.5f * (mdl.bbox_min[2] + mdl.bbox_max[2]) * sc;
    }

    glEnable(GL_DEPTH_TEST);

    /* ---- headless filmstrip: 6 poses of one clip side by side ---- */
    if (shot) {
        int ci = atoi(shot);
        if (ci < 0 || ci >= mdl.clip_count) ci = 0;
        int cols = 6, cw = SW / cols;
        glViewport(0, 0, SW, SH);
        glClearColor(0.10f, 0.11f, 0.13f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        for (int k = 0; k < cols; k++) {
            anim_state_init(&anim);
            if (mdl.clip_count > 0) {
                anim_state_play(&anim, ci, true);
                float t = mdl.clips[ci].duration * (float)k / cols;
                anim_state_update(&anim, &mdl, t > 0 ? t : 1e-6f);
            }
            glViewport(k * cw, 0, cw, SH);
            /* per-column aspect: the projection must match the viewport or
             * the strip squeezes horizontally by the column count */
            mat4 vp; cam_vp(vp, 0.5f, 1.2f, 3.5f, 0.9f, (float)cw / SH);
            grid_draw(prog, vp);
            model_render(&mdl, prog, vp, fit, mdl.clip_count ? &anim : NULL);
        }
        glFinish();
        unsigned char *rgb = malloc((size_t)SW * SH * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        /* glReadPixels rows are bottom-up, exactly what vg_save_bmp expects */
        glReadPixels(0, 0, SW, SH, GL_RGB, GL_UNSIGNED_BYTE, rgb);
        vg_save_bmp("model_view_shot.bmp", SW, SH, rgb);
        free(rgb);
        printf("model_view: wrote model_view_shot.bmp (clip %d)\n", ci);
        model_cleanup(&mdl);
        return 0;
    }

    /* ---- interactive loop ---- */
    float yaw = 0.5f, camh = 1.4f, dist = 4.0f;
    int clip = 0, playing = 1, grid_on = 1;
    uint64_t tprev = SDL_GetTicksNS();
    int run = 1;
    while (run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) run = 0;
            else if (e.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE || k == SDLK_Q) run = 0;
                else if (k == SDLK_TAB && mdl.clip_count > 0) {
                    clip = (clip + 1) % mdl.clip_count;
                    anim_state_play(&anim, clip, true);
                    printf("clip %d: %s\n", clip, mdl.clips[clip].name);
                } else if (k == SDLK_SPACE) playing = !playing;
                else if (k == SDLK_G) grid_on = !grid_on;
                else if (k == SDLK_R) {
                    yaw = 0.5f; camh = 1.4f; dist = 4.0f; clip = 0;
                    anim_state_init(&anim);
                    if (mdl.clip_count > 0) anim_state_play(&anim, 0, true);
                }
            }
        }
        const bool *keys = SDL_GetKeyboardState(NULL);
        uint64_t tnow = SDL_GetTicksNS();
        float dt = (float)(tnow - tprev) / 1e9f;
        tprev = tnow;
        if (dt > 0.1f) dt = 0.1f;
        if (keys[SDL_SCANCODE_LEFT])  yaw -= 1.5f * dt;
        if (keys[SDL_SCANCODE_RIGHT]) yaw += 1.5f * dt;
        if (keys[SDL_SCANCODE_UP])    camh += 1.5f * dt;
        if (keys[SDL_SCANCODE_DOWN])  camh -= 1.5f * dt;
        if (keys[SDL_SCANCODE_MINUS])  dist += 3.0f * dt;
        if (keys[SDL_SCANCODE_EQUALS]) dist -= 3.0f * dt;
        if (dist < 0.5f) dist = 0.5f;

        if (playing && mdl.clip_count > 0)
            anim_state_update(&anim, &mdl, dt);

        glViewport(0, 0, SW, SH);
        glClearColor(0.10f, 0.11f, 0.13f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        mat4 vp; cam_vp(vp, yaw, camh, dist, 0.9f, (float)SW / SH);
        if (grid_on) grid_draw(prog, vp);
        model_render(&mdl, prog, vp, fit, mdl.clip_count ? &anim : NULL);
        SDL_GL_SwapWindow(win);
    }

    model_cleanup(&mdl);
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
