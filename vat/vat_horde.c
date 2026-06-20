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
#include "defense.h"

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

// --- spawn dei nemici come TIPI DI GIOCO (defense.c): la distribuzione decide
// il body (HP/massa/velocità via la tabella EnemyDef), e il body sceglie la
// mesh VAT cosmetica. I crawler NON si spawnano: emergono dai colpi (ferita
// maimed_legs), che li ripinna a CRAWLER_VAR a runtime (vat_layer_set_variant).
static DefBody roll_body(unsigned r){
    unsigned k=r%100u;
    if(k<40)return BT_MAN;   if(k<65)return BT_WOMAN;
    if(k<80)return BT_OBESE; if(k<95)return BT_CHILD;  return BT_TANK; /* 5% */
}
static int body_variant(DefBody b, unsigned r){
    switch(b){ case BT_MAN:return 0; case BT_OBESE:return (r&1)?1:3;
        case BT_WOMAN:return (r&1)?2:5; case BT_CHILD:return 4;
        case BT_TANK:return 1; default:return 0; }     /* tank = obeso scalato */
}
static void spawn_enemy(DefGame *g, SimP *s, VatLayer *vl, float x, float y, unsigned r){
    DefBody b=roll_body(r);
    SimPHandle h=def_spawn(g,x,y,b);
    if(h==SIMP_HANDLE_INVALID)return;
    int i=simp_index_of(s,h); int slot=simp_slot_of(s,i);
    vat_layer_pin_variant(vl,slot,body_variant(b,r>>7));
}

// Benchmark prefill: popola il campo a `target` agenti su un lattice jitterato
// (passo = sqrt(area/target), clampato per non sovrapporre) → niente transitorio
// PBD da spawn denso. Ritorna quanti effettivamente piazzati (free_at salta muri
// e celle piene).
static int prefill_lattice(SimP *s, DefGame *g, VatLayer *vl, const Scene *sc, int target){
    float W=sc->world_w, H=sc->world_h;
    float pitch=sqrtf(W*H/(float)target); if(pitch<0.62f)pitch=0.62f;
    unsigned rng=99; int n=0;
    for(float y=pitch*0.5f; y<H && n<target; y+=pitch)
        for(float x=pitch*0.5f; x<W && n<target; x+=pitch){
            rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;
            float jx=x+((rng%1000)/1000.0f-0.5f)*pitch*0.4f;
            float jy=y+(((rng>>10)%1000)/1000.0f-0.5f)*pitch*0.4f;
            if(simp_free_at(s,jx,jy,0.34f)){ spawn_enemy(g,s,vl,jx,jy,rng>>16); n++; }
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

// --- mesh statica delle torrette: un pilastrino per torretta (arancio = leggera,
// rosso = pesante). Stesso layout 9-float del flat shader (pos, normal, color).
static float *build_turret_mesh(DefGame *g, int *out_nv){
    int nt=def_turret_count(g); int nv=nt*30;
    float *buf=malloc((size_t)nv*9*sizeof(float)); int c=0;
    for(int id=0;id<nt;id++){ DefTurret *t=def_turret(g,id);
        float cx=t->x, cz=t->y, hw=0.32f, h=1.6f;
        float cr=0.95f, cg=t->heavy?0.20f:0.55f, cb=0.10f;
        float x0=cx-hw,x1=cx+hw,z0=cz-hw,z1=cz+hw;
#define VT(PX,PY,PZ,NX,NY,NZ) do{float*o=buf+c*9;o[0]=PX;o[1]=PY;o[2]=PZ;\
        o[3]=NX;o[4]=NY;o[5]=NZ;o[6]=cr;o[7]=cg;o[8]=cb;c++;}while(0)
#define QT(ax,ay,az,bx,by,bz,px2,py2,pz2,dx,dy,dz,nx,ny,nz) do{ \
        VT(ax,ay,az,nx,ny,nz);VT(bx,by,bz,nx,ny,nz);VT(px2,py2,pz2,nx,ny,nz); \
        VT(ax,ay,az,nx,ny,nz);VT(px2,py2,pz2,nx,ny,nz);VT(dx,dy,dz,nx,ny,nz);}while(0)
        QT(x0,h,z0, x1,h,z0, x1,h,z1, x0,h,z1, 0,1,0);    // top
        QT(x1,0,z0, x1,0,z1, x1,h,z1, x1,h,z0, 1,0,0);    // +X
        QT(x0,0,z1, x0,0,z0, x0,h,z0, x0,h,z1, -1,0,0);   // -X
        QT(x0,0,z1, x1,0,z1, x1,h,z1, x0,h,z1, 0,0,1);    // +Z
        QT(x1,0,z0, x0,0,z0, x0,h,z0, x1,h,z0, 0,0,-1);   // -Z
#undef VT
#undef QT
    }
    *out_nv=c; return buf;
}

// --- §7 base & siege (gated on VAT_HORDE_BASE): two concentric wall rings of
// nav cells around the base center, sealing the central goal. The horde sieges
// them (def_update), HP drops, cells free on collapse. State kept file-scope so
// the per-frame mesh rebuild and HUD can read it.
static int gBaseOn=0, gCoreId=-1, gOuterId=-1, gBaseCX=0, gBaseCY=0, gBaseHO=0;
static void build_base(DefGame *g, SimP *s, float cell, float bcx, float bcy){
    int cx0=(int)(bcx/cell), cy0=(int)(bcy/cell), hc=6, ho=16;
    gCoreId  = def_add_structure(g, 1200.0f, 1);   // innermost = loss
    gOuterId = def_add_structure(g,  800.0f, 0);   // reroutes on collapse
    for(int cy=cy0-hc;cy<=cy0+hc;cy++)for(int cx=cx0-hc;cx<=cx0+hc;cx++)
        if(cx==cx0-hc||cx==cx0+hc||cy==cy0-hc||cy==cy0+hc) def_struct_cell(g,gCoreId,cx,cy);
    for(int cy=cy0-ho;cy<=cy0+ho;cy++)for(int cx=cx0-ho;cx<=cx0+ho;cx++)
        if(cx==cx0-ho||cx==cx0+ho||cy==cy0-ho||cy==cy0+ho) def_struct_cell(g,gOuterId,cx,cy);
    simp_terrain_commit(s);
    gBaseCX=cx0; gBaseCY=cy0; gBaseHO=ho; gBaseOn=1;
}
// rebuild the live structure mesh each frame from def_cell_struct: collapsed
// cells vanish, surviving cells darken as their structure's HP drops. A box per
// live cell, 9-float flat layout. Returns vertex count.
static int build_struct_mesh(DefGame *g, float cell, float *buf){
    int c=0; float H=2.8f;
    for(int cy=gBaseCY-gBaseHO-1; cy<=gBaseCY+gBaseHO+1; cy++)
    for(int cx=gBaseCX-gBaseHO-1; cx<=gBaseCX+gBaseHO+1; cx++){
        int id=def_cell_struct(g,cx,cy); if(id<0) continue;
        float frac=def_struct_hp(g,id)/ (def_struct_hp_max(g,id)+1e-3f); // 1..0
        float t=0.35f+0.65f*frac;                         // darken with damage
        float cr,cg,cb;
        if(id==gCoreId){ cr=0.75f*t; cg=0.16f*t; cb=0.16f*t; }   // core = red
        else           { cr=0.55f*t; cg=0.57f*t; cb=0.62f*t; }   // ring = steel
        float x0=cx*cell, x1=x0+cell, z0=cy*cell, z1=z0+cell;
#define VS(PX,PY,PZ,NX,NY,NZ) do{float*o=buf+c*9;o[0]=PX;o[1]=PY;o[2]=PZ;\
        o[3]=NX;o[4]=NY;o[5]=NZ;o[6]=cr;o[7]=cg;o[8]=cb;c++;}while(0)
#define QS(ax,ay,az,bx,by,bz,px2,py2,pz2,dx,dy,dz,nx,ny,nz) do{ \
        VS(ax,ay,az,nx,ny,nz);VS(bx,by,bz,nx,ny,nz);VS(px2,py2,pz2,nx,ny,nz); \
        VS(ax,ay,az,nx,ny,nz);VS(px2,py2,pz2,nx,ny,nz);VS(dx,dy,dz,nx,ny,nz);}while(0)
        QS(x0,H,z0, x1,H,z0, x1,H,z1, x0,H,z1, 0,1,0);    // top
        QS(x1,0,z0, x1,0,z1, x1,H,z1, x1,H,z0, 1,0,0);    // +X
        QS(x0,0,z1, x0,0,z0, x0,H,z0, x0,H,z1, -1,0,0);   // -X
        QS(x0,0,z1, x1,0,z1, x1,H,z1, x0,H,z1, 0,0,1);    // +Z
        QS(x1,0,z0, x0,0,z0, x0,H,z0, x1,H,z0, 0,0,-1);   // -Z
#undef VS
#undef QS
    }
    return c;
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

    // gameplay difensivo (M5): torrette in anello attorno alla base (centroide
    // dei goal, fallback centro mondo), affacciate verso l'esterno.
    DefGame *g = def_create(s, MAXA);
    float bcx=sc.world_w*0.5f, bcy=sc.world_h*0.5f;
    if(sc.n_goal>0){ float sx=0,sy=0; for(int k=0;k<sc.n_goal;k++){
            sx+=sc.goal[k].x+sc.goal[k].w*0.5f; sy+=sc.goal[k].y+sc.goal[k].h*0.5f; }
        bcx=sx/sc.n_goal; bcy=sy/sc.n_goal; }
#define NT 10
    float mn = sc.world_w<sc.world_h?sc.world_w:sc.world_h;
    float TR_R = 0.22f*mn;
    int nt_want=getenv("VAT_HORDE_TURRETS")?atoi(getenv("VAT_HORDE_TURRETS")):NT;
    if(nt_want>NT)nt_want=NT; if(nt_want<0)nt_want=0;
    int placed=0;
    for(int i=0;i<nt_want;i++){ float th=(float)i*(6.2831853f/(float)NT);
        float tx=bcx+TR_R*cosf(th), ty=bcy+TR_R*sinf(th);
        /* salta le posizioni fuori dal mondo (base sul bordo → mezzo anello
           cadrebbe fuori): restano le torrette che guardano l'orda in arrivo */
        if(tx<1.0f||tx>sc.world_w-1.0f||ty<1.0f||ty>sc.world_h-1.0f) continue;
        DefTurret t={0};
        t.x=tx; t.y=ty; t.ang=th;
        /* arco ampio = auto-target sul più vicino (ingaggio garantito per questo
           primo aggancio; l'arco direzionale stretto è tuning gameplay futuro) */
        float ha=3.15f; t.arc_min=th-ha; t.arc_max=th+ha;
        t.sweep_dir=1; t.sweep_speed=3.0f; t.range=55.0f;
        t.heavy=(i%4==2); t.piercing=(i%4==0);
        t.fire_period=t.heavy?0.5f:0.10f; t.damage=t.heavy?0.0f:55.0f;
        def_add_turret(g,&t); placed++; }
    printf("torrette: %d piazzate (anello r=%.1f m attorno alla base (%.1f,%.1f))\n",
           placed,(double)TR_R,(double)bcx,(double)bcy);

    // §7 base & siege: due anelli distruttibili attorno al goal centrale.
    if(getenv("VAT_HORDE_BASE")){ build_base(g,s,sc.cell,bcx,bcy);
        printf("base: core HP %.0f + ring HP %.0f attorno a cella (%d,%d)\n",
               (double)def_struct_hp_max(g,gCoreId),(double)def_struct_hp_max(g,gOuterId),
               gBaseCX,gBaseCY); }

    if(fillN){ int got=prefill_lattice(s,g,vl,&sc,fillN);
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

    // torrette (statico) + tracer di fuoco (dinamico), stesso flat shader.
    int turNV=0; float *turMesh=build_turret_mesh(g,&turNV);
    GLuint turVao,turVbo; glGenVertexArrays(1,&turVao);glBindVertexArray(turVao);
    glGenBuffers(1,&turVbo);glBindBuffer(GL_ARRAY_BUFFER,turVbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)turNV*9*sizeof(float),turMesh,GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,9*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,0,9*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,9*sizeof(float),(void*)(6*sizeof(float)));glEnableVertexAttribArray(2);
    glBindVertexArray(0); free(turMesh);
    GLuint trVao,trVbo; glGenVertexArrays(1,&trVao);glBindVertexArray(trVao);
    glGenBuffers(1,&trVbo);glBindBuffer(GL_ARRAY_BUFFER,trVbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)NT*2*9*sizeof(float),NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,9*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,0,9*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,9*sizeof(float),(void*)(6*sizeof(float)));glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    // strutture della base (dinamico: ricostruito ogni frame dallo stato vivo).
    int stMaxV = gBaseOn ? (2*gBaseHO+3)*(2*gBaseHO+3)*30 : 0;
    float *stBuf = stMaxV ? malloc((size_t)stMaxV*9*sizeof(float)) : NULL;
    GLuint stVao=0,stVbo=0;
    if(gBaseOn){ glGenVertexArrays(1,&stVao);glBindVertexArray(stVao);
        glGenBuffers(1,&stVbo);glBindBuffer(GL_ARRAY_BUFFER,stVbo);
        glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)stMaxV*9*sizeof(float),NULL,GL_DYNAMIC_DRAW);
        glVertexAttribPointer(0,3,GL_FLOAT,0,9*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
        glVertexAttribPointer(1,3,GL_FLOAT,0,9*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
        glVertexAttribPointer(2,3,GL_FLOAT,0,9*sizeof(float),(void*)(6*sizeof(float)));glEnableVertexAttribArray(2);
        glBindVertexArray(0); }

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
    unsigned rng=1234;
    Uint64 pf=SDL_GetPerformanceFrequency();
    // timestep FISSO disaccoppiato dal framerate: accumulo il tempo reale e lo
    // consumo in passi da 1/60 → la sim avanza alla stessa velocità a 60 o 144 Hz
    // (il vsync governa i frame, non i passi di simulazione).
    const float FIXED_DT=1.0f/60.0f;
    Uint64 prev=SDL_GetPerformanceCounter(); double acc_t=0.0;
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
        double sim_ms=0, lay_ms=0;
        // tempo del frame: reale (interattivo) o fisso 1/60 (headless shot/bench →
        // 1 passo/frame, deterministico e indipendente dal tempo a parete).
        Uint64 nowc=SDL_GetPerformanceCounter();
        double frame_t=(double)(nowc-prev)/(double)pf; prev=nowc;
        if(shot || bench_meas) frame_t=FIXED_DT;
        if(frame_t>0.25) frame_t=0.25;                 // anti spiral-of-death
        if(!paused){
            acc_t+=frame_t;
            while(acc_t>=FIXED_DT){
                // spawn dalle rect (burst-free via free_at). In benchmark (fillN)
                // il campo è già pieno: niente spawn per-passo.
                for(int k=0;!fillN && k<30 && simp_count(s)<MAXA;k++){
                    rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;
                    const SceneRect *r=&sc.spawn[(rng>>3)%sc.n_spawn];
                    float x=r->x+(rng%10000)/10000.0f*r->w;
                    float y=r->y+((rng>>8)%10000)/10000.0f*r->h;
                    if(simp_free_at(s,x,y,0.34f)) spawn_enemy(g,s,vl,x,y,rng>>16);
                }
                Uint64 t0=SDL_GetPerformanceCounter();
                simp_step(s,FIXED_DT);
                Uint64 t1=SDL_GetPerformanceCounter();
                def_update(g,FIXED_DT);
                // i feriti maimed_legs passano alla mesh crawler (a runtime)
                { const uint8_t *wnd=def_wound(g); int nn=simp_count(s);
                  for(int i=0;i<nn;i++){ int slot=simp_slot_of(s,i);
                      if(wnd[slot]==DW_CRAWLING) vat_layer_set_variant(vl,slot,CRAWLER_VAR); } }
                vat_layer_update(vl,s,FIXED_DT);
                Uint64 t2=SDL_GetPerformanceCounter();
                sim_ms+=(double)(t1-t0)*1000.0/pf;
                lay_ms+=(double)(t2-t1)*1000.0/pf;
                acc_t-=FIXED_DT;
            }
        } else acc_t=0.0;                              // in pausa: niente debito

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
        // torrette (pilastrini statici)
        glBindVertexArray(turVao);glDrawArrays(GL_TRIANGLES,0,turNV);
        // tracer di fuoco (linee, una per torretta che ha sparato di recente)
        { float trbuf[NT*2*9]; int tc=0;
          for(int id=0;id<def_turret_count(g);id++){ DefTurret *t=def_turret(g,id);
              if(t->tracer_ttl<=0.0f) continue;
              float ex=t->x+cosf(t->ang)*t->last_t, ez=t->y+sinf(t->ang)*t->last_t;
              float *o=trbuf+tc*9; o[0]=t->x;o[1]=0.9f;o[2]=t->y;
              o[3]=0;o[4]=1;o[5]=0; o[6]=3;o[7]=3;o[8]=0.4f; tc++;
              o=trbuf+tc*9; o[0]=ex;o[1]=0.9f;o[2]=ez;
              o[3]=0;o[4]=1;o[5]=0; o[6]=3;o[7]=3;o[8]=0.4f; tc++; }
          if(tc){ glBindVertexArray(trVao);glBindBuffer(GL_ARRAY_BUFFER,trVbo);
              glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)tc*9*sizeof(float),trbuf);
              glLineWidth(2.5f); glDrawArrays(GL_LINES,0,tc); } }

        // strutture della base (rebuild dallo stato vivo: celle crollate spariscono)
        if(gBaseOn){ int sv=build_struct_mesh(g,sc.cell,stBuf);
            if(sv){ glBindVertexArray(stVao);glBindBuffer(GL_ARRAY_BUFFER,stVbo);
                glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)sv*9*sizeof(float),stBuf);
                glDrawArrays(GL_TRIANGLES,0,sv); } }

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
        if(acc_n>=30){ char title[320]; double S=acc_sim/acc_n,L=acc_lay/acc_n,R=acc_ren/acc_n;
            char base[96]=""; if(gBaseOn){ int pc=(int)(100.0f*def_struct_hp(g,gCoreId)/(def_struct_hp_max(g,gCoreId)+1e-3f));
                int po=(int)(100.0f*def_struct_hp(g,gOuterId)/(def_struct_hp_max(g,gOuterId)+1e-3f));
                snprintf(base,sizeof base, def_lost(g)?" | BASE PERSA":" | ring %d%% core %d%%", po<0?0:po, pc<0?0:pc); }
            snprintf(title,sizeof title,"vat_horde — %d agenti | kills %d crawler %d%s | sim %.2f lay %.2f ren %.2f ms | %.0f fps",
                     total,def_kills(g),def_count_wound(g,DW_CRAWLING),base,S,L,R,1000.0/(S+L+R));
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
            printf("frame %d: %d agenti | shots %d kills %d, crawler %d, bloody %d, arm %d | sim %.2f render %.2f ms -> vat_horde_shot.bmp\n",
                   frame,total,def_shots(g),def_kills(g),def_count_wound(g,DW_CRAWLING),
                   def_count_wound(g,DW_BLOODY),def_count_wound(g,DW_MAIMED_ARM),sim_ms,ren_ms);
            if(gBaseOn) printf("  base: ring HP %.0f/%.0f%s | core HP %.0f/%.0f%s | %s\n",
                   (double)def_struct_hp(g,gOuterId),(double)def_struct_hp_max(g,gOuterId),
                   def_struct_collapsed(g,gOuterId)?" CROLLATO":"",
                   (double)def_struct_hp(g,gCoreId),(double)def_struct_hp_max(g,gCoreId),
                   def_struct_collapsed(g,gCoreId)?" CROLLATO":"", def_lost(g)?"BASE PERSA":"base regge");
            shot_done=1; running=0; }
        SDL_GL_SwapWindow(win);
    }
    free(stBuf);
    def_destroy(g); vat_layer_destroy(vl); simp_destroy(s); scene_free(&sc);
    SDL_GL_DestroyContext(ctx);SDL_DestroyWindow(win);SDL_Quit(); return 0;
}
