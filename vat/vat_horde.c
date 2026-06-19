// vat_horde — l'orda reale del core sim_particles resa in 3D VAT, MULTI-MODELLO
// (migrazione_3d.md §Multi-modello) su SCENA VETTORIALE (scene.h): gli ostacoli
// sono poligoni convessi rasterizzati nella nav (muro/costo) ed estrusi a una
// loro altezza nel render (flat-shaded). Il render layer vat_layer guida
// heading/FSM/fase/outfit e assegna il BODY per slot; il renderer fa una
// glDrawElementsInstanced per variante.
//
//   ./vat_horde [scena.scn]            (default: scenes/obstacles.scn)
// Controlli: frecce=pan  +/-=zoom  C=camera  T=texture  SPACE=pausa  ESC=esci
// Headless:  VAT_HORDE_SHOT="<frames>" ./vat_horde  -> simula N step, screenshot
//            vat_horde_shot.bmp, esce. VAT_HORDE_CAM="cx,cz,hh,az,el".
#include <SDL3/SDL.h>
#include "vat_gl.h"
#include "vat_layer.h"
#include "sim_particles.h"
#include "scene.h"

#define MAXA 60000
static float inst[MAXA*12];

// I body type disponibili (asset bakati in vat/assets/). Texture placeholder: la
// skirt non ha ancora il _diffuse.png -> rende flat-shaded (tintata), corretto.
static const char *PREFIX[] = {
    "vat/assets/zombie_man", "vat/assets/zombie_man_obese",
    "vat/assets/zombie_fem", "vat/assets/zombie_fem_obese",
    "vat/assets/zombie_child", "vat/assets/zombie_fem_skirt",
    "vat/assets/zombie_maimed_legs",   /* crawler: tipo di gioco, non cosmetico */
};
#define NVAR ((int)(sizeof(PREFIX)/sizeof(PREFIX[0])))
#define CRAWLER_VAR (NVAR-1)               /* indice del body crawler */
#define NCOSMETIC   (NVAR-1)               /* le altre = body random cosmetici */

/* crawler: striscia lento; il body viene pinnato a CRAWLER_VAR allo spawn */
static const SimPAgentDesc CRAWLER = {0.30f, 0.6f, 1.0f};

// Benchmark prefill: popola il campo a `target` agenti su un lattice jitterato
// (passo = sqrt(area/target), clampato per non sovrapporre) → niente transitorio
// PBD da spawn denso. Ritorna quanti effettivamente piazzati (free_at salta muri
// e celle piene). ~80% walker, 20% runner.
static int prefill_lattice(SimP *s, VatLayer *vl, const Scene *sc, int target){
    float W=sc->world_w, H=sc->world_h;
    float pitch=sqrtf(W*H/(float)target); if(pitch<0.62f)pitch=0.62f;
    SimPAgentDesc runner={0.30f,3.6f,1.0f};
    unsigned rng=99; int n=0;
    for(float y=pitch*0.5f; y<H && n<target; y+=pitch)
        for(float x=pitch*0.5f; x<W && n<target; x+=pitch){
            rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;
            float jx=x+((rng%1000)/1000.0f-0.5f)*pitch*0.4f;
            float jy=y+(((rng>>10)%1000)/1000.0f-0.5f)*pitch*0.4f;
            if(simp_free_at(s,jx,jy,0.34f)){
                unsigned roll=(rng>>20)%100;
                if(roll<15){ int i=simp_spawn_desc(s,jx,jy,&CRAWLER);
                             if(i>=0) vat_layer_pin_variant(vl,simp_slot_of(s,i),CRAWLER_VAR); }
                else if(roll<35) simp_spawn_desc(s,jx,jy,&runner);
                else simp_spawn(s,jx,jy);
                n++;
            }
        }
    return n;
}

typedef struct { GLuint vao, texP, texN, texD; int ni, hasTex; const VatMeta *M; } Asset;

// --- mesh statica degli ostacoli: ogni poligono estruso (top + pareti) + suolo.
// 9 float/vertice: pos.xyz, normal.xyz, color.rgb. World = (sim_x, altezza, sim_y).
static float *build_obstacle_mesh(const Scene *sc, int *out_nverts) {
    int nv = 6;                                  // ground quad
    for (int k = 0; k < sc->n_poly; k++) { int n = sc->poly[k].nverts; nv += (n - 2) * 3 + n * 6; }
    float *buf = malloc((size_t)nv * 9 * sizeof(float));
    int c = 0;
#define PUSH(PX,PY,PZ,NX,NY,NZ,R,G,B) do{ float*o=buf+c*9; \
    o[0]=(PX);o[1]=(PY);o[2]=(PZ); o[3]=(NX);o[4]=(NY);o[5]=(NZ); o[6]=(R);o[7]=(G);o[8]=(B); c++; }while(0)
    // suolo (leggermente sotto 0 per non z-fightare con le basi)
    float W = sc->world_w, H = sc->world_h, gy = -0.02f;
    PUSH(0,gy,0, 0,1,0, 0.16f,0.18f,0.15f); PUSH(W,gy,0, 0,1,0, 0.16f,0.18f,0.15f); PUSH(W,gy,H, 0,1,0, 0.16f,0.18f,0.15f);
    PUSH(0,gy,0, 0,1,0, 0.16f,0.18f,0.15f); PUSH(W,gy,H, 0,1,0, 0.16f,0.18f,0.15f); PUSH(0,gy,H, 0,1,0, 0.16f,0.18f,0.15f);

    for (int k = 0; k < sc->n_poly; k++) {
        const ScenePoly *p = &sc->poly[k];
        int n = p->nverts; float h = p->height;
        float cr = p->solid ? 0.56f : 0.46f, cg = p->solid ? 0.56f : 0.34f, cb = p->solid ? 0.60f : 0.20f;
        float ccx = 0, ccz = 0;
        for (int i = 0; i < n; i++) { ccx += p->vx[i]; ccz += p->vy[i]; }
        ccx /= n; ccz /= n;
        // top face (triangle fan, convesso)
        for (int i = 1; i + 1 < n; i++) {
            PUSH(p->vx[0],h,p->vy[0], 0,1,0, cr,cg,cb);
            PUSH(p->vx[i],h,p->vy[i], 0,1,0, cr,cg,cb);
            PUSH(p->vx[i+1],h,p->vy[i+1], 0,1,0, cr,cg,cb);
        }
        // pareti
        for (int k0 = 0; k0 < n; k0++) {
            int k1 = (k0 + 1) % n;
            float dx = p->vx[k1] - p->vx[k0], dz = p->vy[k1] - p->vy[k0];
            float nx = dz, nz = -dx;             // perpendicolare nel piano XZ
            float mx = (p->vx[k0] + p->vx[k1]) * 0.5f, mz = (p->vy[k0] + p->vy[k1]) * 0.5f;
            if (nx * (mx - ccx) + nz * (mz - ccz) < 0) { nx = -nx; nz = -nz; } // verso l'esterno
            float l = sqrtf(nx*nx + nz*nz); if (l > 1e-6f) { nx /= l; nz /= l; }
            float shade = 0.85f;                 // pareti un filo più scure del top
            float wr = cr*shade, wg = cg*shade, wb = cb*shade;
            PUSH(p->vx[k0],h,p->vy[k0], nx,0,nz, wr,wg,wb);
            PUSH(p->vx[k1],h,p->vy[k1], nx,0,nz, wr,wg,wb);
            PUSH(p->vx[k1],0,p->vy[k1], nx,0,nz, wr,wg,wb);
            PUSH(p->vx[k0],h,p->vy[k0], nx,0,nz, wr,wg,wb);
            PUSH(p->vx[k1],0,p->vy[k1], nx,0,nz, wr,wg,wb);
            PUSH(p->vx[k0],0,p->vy[k0], nx,0,nz, wr,wg,wb);
        }
    }
#undef PUSH
    *out_nverts = c;
    return buf;
}

int main(int argc, char **argv){
    const char *scene_path = argc > 1 ? argv[1] : "scenes/obstacles.scn";
    Scene sc;
    if (scene_load(scene_path, &sc) != 0) { fprintf(stderr, "scene load fail: %s\n", scene_path); return 1; }
    printf("scene %s: %dx%d cell=%g (%gx%g m) poly=%d spawn=%d goal=%d\n",
           scene_path, sc.gw, sc.gh, (double)sc.cell, (double)sc.world_w, (double)sc.world_h,
           sc.n_poly, sc.n_spawn, sc.n_goal);
    // Modalità benchmark: VAT_HORDE_FILL=N prefilla il campo; VAT_HORDE_BENCH=
    // "warmup,measure" scalda poi misura medie sim/render su `measure` step.
    int fillN = getenv("VAT_HORDE_FILL") ? atoi(getenv("VAT_HORDE_FILL")) : 0;
    if (fillN > MAXA) fillN = MAXA;
    int bench_warm=0, bench_meas=0;
    if (getenv("VAT_HORDE_BENCH")) { sscanf(getenv("VAT_HORDE_BENCH"), "%d,%d", &bench_warm, &bench_meas);
        if (bench_meas <= 0) bench_meas = 300; }
    if (sc.n_spawn == 0 && !fillN) { fprintf(stderr, "scene ha 0 spawn rect\n"); return 1; }

    char metas[NVAR][256];
    const char *metap[NVAR];
    for(int v=0;v<NVAR;v++){ snprintf(metas[v],256,"%s_meta.txt",PREFIX[v]); metap[v]=metas[v]; }

    VatLayer *vl=vat_layer_create_multi(metap,NVAR,MAXA);
    for(int v=0;v<NVAR;v++){ const VatMeta *M=vat_layer_meta_variant(vl,v);
        if(M->nclips<=0){fprintf(stderr,"meta vuoto: %s\n",PREFIX[v]);return 1;} }
    vat_layer_set_random_count(vl,NCOSMETIC);   /* il crawler solo via pin, non a caso */
    printf("varianti=%d (di cui %d cosmetiche + crawler)\n",NVAR,NCOSMETIC);

    SimP *s = scene_instantiate(&sc, MAXA);
    if(!s){ fprintf(stderr,"scene_instantiate fail\n"); return 1; }
    if(fillN){ int got=prefill_lattice(s,vl,&sc,fillN);
        printf("prefill: target %d -> %d agenti piazzati\n", fillN, got); }

    const char *shot=getenv("VAT_HORDE_SHOT");
    int shot_frames = shot? atoi(shot):0; if(shot&&shot_frames<=0)shot_frames=600;
    int SW=1280,SH=720;

    if(!SDL_Init(SDL_INIT_VIDEO)){fprintf(stderr,"SDL:%s\n",SDL_GetError());return 1;}
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,24);
    SDL_Window*win=SDL_CreateWindow("vat_horde",SW,SH,SDL_WINDOW_OPENGL);
    SDL_GLContext ctx=SDL_GL_CreateContext(win);
    if(!ctx||!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)){fprintf(stderr,"GL init fail\n");return 1;}
    printf("GL %s\n",glGetString(GL_VERSION));

    GLuint prog=vg_shader("vat/vat.vs","vat/vat.fs");
    // instance buffer condiviso: ri-riempito per variante prima di ogni draw.
    GLuint bi; glGenBuffers(1,&bi);glBindBuffer(GL_ARRAY_BUFFER,bi);
    glBufferData(GL_ARRAY_BUFFER,sizeof(inst),NULL,GL_DYNAMIC_DRAW);

    // --- ostacoli: programma flat + mesh statica estrusa dalla scena.
    GLuint progFlat=vg_shader("vat/flat.vs","vat/flat.fs");
    GLint uVPflat=glGetUniformLocation(progFlat,"uVP");
    int obNV=0; float *obMesh=build_obstacle_mesh(&sc,&obNV);
    GLuint obVao,obVbo; glGenVertexArrays(1,&obVao);glBindVertexArray(obVao);
    glGenBuffers(1,&obVbo);glBindBuffer(GL_ARRAY_BUFFER,obVbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)obNV*9*sizeof(float),obMesh,GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,9*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,0,9*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,9*sizeof(float),(void*)(6*sizeof(float)));glEnableVertexAttribArray(2);
    glBindVertexArray(0); free(obMesh);
    printf("ostacoli: %d triangoli\n",obNV/3);

    // carica un Asset (VAO+mesh+texture VAT) per variante.
    Asset A[NVAR];
    for(int v=0;v<NVAR;v++){
        const VatMeta *M=vat_layer_meta_variant(vl,v); A[v].M=M;
        char pos[256],norm[256],mesh[256],diff[256];
        snprintf(pos,256,"%s_pos.raw",PREFIX[v]);snprintf(norm,256,"%s_norm.raw",PREFIX[v]);
        snprintf(mesh,256,"%s_mesh.bin",PREFIX[v]);snprintf(diff,256,"%s_diffuse.png",PREFIX[v]);

        FILE*mf=fopen(mesh,"rb"); if(!mf){fprintf(stderr,"no mesh %s\n",mesh);return 1;}
        int nv,ni; if(fread(&nv,4,1,mf)){}if(fread(&ni,4,1,mf)){}
        float*verts=malloc(nv*3*4),*uvs=malloc(nv*2*4); unsigned short*idx=malloc(ni*2);
        if(fread(verts,4,nv*3,mf)){}if(fread(uvs,4,nv*2,mf)){}if(fread(idx,2,ni,mf)){}fclose(mf);
        A[v].ni=ni;
        A[v].texP=vg_tex_raw(pos,M->texW,M->texH);
        A[v].texN=vg_tex_raw(norm,M->texW,M->texH);
        A[v].texD=vg_tex_png(diff); A[v].hasTex=A[v].texD!=0;

        GLuint vao,bp,bu,eb; glGenVertexArrays(1,&vao);glBindVertexArray(vao);
        glGenBuffers(1,&bp);glBindBuffer(GL_ARRAY_BUFFER,bp);glBufferData(GL_ARRAY_BUFFER,nv*3*4,verts,GL_STATIC_DRAW);
        glVertexAttribPointer(0,3,GL_FLOAT,0,0,0);glEnableVertexAttribArray(0);
        glGenBuffers(1,&bu);glBindBuffer(GL_ARRAY_BUFFER,bu);glBufferData(GL_ARRAY_BUFFER,nv*2*4,uvs,GL_STATIC_DRAW);
        glVertexAttribPointer(1,2,GL_FLOAT,0,0,0);glEnableVertexAttribArray(1);
        glGenBuffers(1,&eb);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,eb);glBufferData(GL_ELEMENT_ARRAY_BUFFER,ni*2,idx,GL_STATIC_DRAW);
        // attributi istanza dal buffer condiviso bi (stesso layout per tutte le mesh)
        glBindBuffer(GL_ARRAY_BUFFER,bi);
        for(int i=0;i<3;i++){glEnableVertexAttribArray(2+i);
            glVertexAttribPointer(2+i,4,GL_FLOAT,0,12*sizeof(float),(void*)(i*4*sizeof(float)));glVertexAttribDivisor(2+i,1);}
        glBindVertexArray(0); A[v].vao=vao;
        free(verts);free(uvs);free(idx);
    }
    glEnable(GL_DEPTH_TEST);
    GLint uVP=glGetUniformLocation(prog,"uVP"),uTS=glGetUniformLocation(prog,"texSize"),uRPF=glGetUniformLocation(prog,"rowsPerFrame"),uHas=glGetUniformLocation(prog,"uHasTex");

    float cx=sc.world_w*0.5f, cz=sc.world_h*0.30f, hh=15.0f, az=0.7f, el=0.40f; int cam_free=0,paused=0,useTex=1;
    { const char*cs=getenv("VAT_HORDE_CAM"); if(cs) sscanf(cs,"%f,%f,%f,%f,%f",&cx,&cz,&hh,&az,&el); }
    SimPAgentDesc runner={0.30f,3.6f,1.0f};
    unsigned rng=1234;
    Uint64 pf=SDL_GetPerformanceFrequency();
    double acc_sim=0,acc_lay=0,acc_ren=0; int acc_n=0;
    double bsim=0,blay=0,bren=0; int bn=0;     // finestra di misura benchmark
    int running=1,frame=0,shot_done=0;
    while(running){
        SDL_Event e; while(SDL_PollEvent(&e)){ if(e.type==SDL_EVENT_QUIT)running=0;
            else if(e.type==SDL_EVENT_KEY_DOWN)switch(e.key.key){
                case SDLK_ESCAPE:running=0;break; case SDLK_C:cam_free=!cam_free;break; case SDLK_T:useTex=!useTex;break;
                case SDLK_SPACE:paused=!paused;break;
                case SDLK_LEFT:az-=0.06f;break; case SDLK_RIGHT:az+=0.06f;break; case SDLK_UP:el+=0.04f;break; case SDLK_DOWN:el-=0.04f;break;
                case SDLK_EQUALS:case SDLK_PLUS:hh*=0.9f;break; case SDLK_MINUS:hh*=1.1f;break; } }
        float dt=1.0f/60.0f;
        double sim_ms=0, lay_ms=0;
        if(!paused){
            // spawn dalle rect di spawn (burst-free via free_at), ~80% walker 20% runner.
            // In benchmark (fillN) il campo è già pieno: niente spawn per-frame.
            for(int k=0;!fillN && k<60 && simp_count(s)<MAXA;k++){
                rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;
                const SceneRect *r=&sc.spawn[(rng>>3)%sc.n_spawn];
                float x=r->x+(rng%10000)/10000.0f*r->w;
                float y=r->y+((rng>>8)%10000)/10000.0f*r->h;
                if(simp_free_at(s,x,y,0.34f)){
                    unsigned roll=(rng>>20)%100;
                    if(roll<15){ int i=simp_spawn_desc(s,x,y,&CRAWLER);
                                 if(i>=0) vat_layer_pin_variant(vl,simp_slot_of(s,i),CRAWLER_VAR); }
                    else if(roll<35) simp_spawn_desc(s,x,y,&runner);
                    else simp_spawn(s,x,y);
                }
            }
            Uint64 t0=SDL_GetPerformanceCounter();
            simp_step(s,dt);
            Uint64 t1=SDL_GetPerformanceCounter();
            vat_layer_update(vl,s,dt);
            Uint64 t2=SDL_GetPerformanceCounter();
            sim_ms=(double)(t1-t0)*1000.0/pf;
            lay_ms=(double)(t2-t1)*1000.0/pf;
        }

        float asp=(float)SW/SH; mat4 proj,view,vp; float ctr[3]={cx,0.9f,cz},up[3]={0,1,0};
        float eye[3]={cx+hh*cosf(el)*sinf(az),0.9f+hh*sinf(el),cz+hh*cosf(el)*cosf(az)};
        if(cam_free)m_persp(proj,45.0f*3.14159f/180.0f,asp,0.1f,500.0f);
        else m_ortho(proj,-hh*asp,hh*asp,-hh,hh,-200,400);
        m_lookat(view,eye,ctr,up); m_mul(vp,proj,view);

        Uint64 r0=SDL_GetPerformanceCounter();
        glViewport(0,0,SW,SH);glClearColor(0.12f,0.13f,0.16f,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

        // ostacoli + suolo (statici)
        glUseProgram(progFlat);glUniformMatrix4fv(uVPflat,1,GL_FALSE,vp);
        glBindVertexArray(obVao);glDrawArrays(GL_TRIANGLES,0,obNV);

        // orda VAT
        glUseProgram(prog);glUniformMatrix4fv(uVP,1,GL_FALSE,vp);
        glUniform1i(glGetUniformLocation(prog,"texPos"),0);
        glUniform1i(glGetUniformLocation(prog,"texNorm"),1);
        glUniform1i(glGetUniformLocation(prog,"texDiff"),2);

        int total=0;
        for(int v=0;v<NVAR;v++){
            int count=vat_layer_fill_variant(vl,s,v,inst,MAXA);
            if(!count) continue; total+=count;
            const VatMeta *M=A[v].M;
            glBindBuffer(GL_ARRAY_BUFFER,bi);glBufferSubData(GL_ARRAY_BUFFER,0,count*12*sizeof(float),inst);
            glUniform2f(uTS,(float)M->texW,(float)M->texH);glUniform1f(uRPF,(float)M->rowsPerFrame);
            glUniform1i(uHas,useTex&&A[v].hasTex);
            glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,A[v].texP);
            glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,A[v].texN);
            glActiveTexture(GL_TEXTURE2);glBindTexture(GL_TEXTURE_2D,A[v].texD);
            glBindVertexArray(A[v].vao);glDrawElementsInstanced(GL_TRIANGLES,A[v].ni,GL_UNSIGNED_SHORT,0,count);
        }
        glBindVertexArray(0);
        glFinish();
        double ren_ms=(double)(SDL_GetPerformanceCounter()-r0)*1000.0/pf;

        // HUD: medie mobili sim/layer/render nel titolo
        acc_sim+=sim_ms; acc_lay+=lay_ms; acc_ren+=ren_ms; acc_n++;
        if(acc_n>=30){ char title[200]; double S=acc_sim/acc_n,L=acc_lay/acc_n,R=acc_ren/acc_n;
            snprintf(title,sizeof title,"vat_horde — %d agenti | sim %.2f | layer %.2f | render %.2f ms | %.0f fps-cap",
                     total,S,L,R,1000.0/(S+L+R));
            SDL_SetWindowTitle(win,title); acc_sim=acc_lay=acc_ren=0; acc_n=0; }

        // benchmark: accumula nella finestra di misura, poi stampa medie ed esci
        if(bench_meas){
            if(frame>=bench_warm){ bsim+=sim_ms; blay+=lay_ms; bren+=ren_ms; bn++; }
            if(frame+1>=bench_warm+bench_meas){
                double si=bsim/bn, la=blay/bn, re=bren/bn, tot=si+la+re;
                printf("BENCH fill=%d agenti=%d | core_sim %.2f | vat_layer %.2f | render %.2f ms | tot %.2f ms (cap %.0f fps)\n",
                       fillN, total, si, la, re, tot, 1000.0/tot);
                glFinish(); unsigned char*px=malloc(SW*SH*3); glReadPixels(0,0,SW,SH,GL_RGB,GL_UNSIGNED_BYTE,px);
                vg_save_bmp("vat_horde_shot.bmp",SW,SH,px); free(px);
                running=0;
            }
        }

        frame++;
        if(shot && !shot_done && frame>=shot_frames){ glFinish();
            unsigned char*px=malloc(SW*SH*3); glReadPixels(0,0,SW,SH,GL_RGB,GL_UNSIGNED_BYTE,px);
            vg_save_bmp("vat_horde_shot.bmp",SW,SH,px); free(px);
            printf("frame %d: %d agenti (%d varianti) | sim %.2f ms render %.2f ms -> vat_horde_shot.bmp\n",
                   frame,total,NVAR,sim_ms,ren_ms);
            shot_done=1; running=0; }
        SDL_GL_SwapWindow(win);
    }
    vat_layer_destroy(vl); simp_destroy(s); scene_free(&sc);
    SDL_GL_DestroyContext(ctx);SDL_DestroyWindow(win);SDL_Quit(); return 0;
}
