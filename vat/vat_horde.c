// vat_horde — l'orda reale del core sim_particles resa in 3D VAT, MULTI-MODELLO
// (migrazione_3d.md §Multi-modello) su SCENA VETTORIALE (scene.h): gli ostacoli
// sono poligoni convessi rasterizzati nella nav (muro/costo) ed estrusi a una
// loro altezza nel render (flat-shaded). Il render layer vat_layer guida
// heading/FSM/fase/outfit e assegna il BODY per slot; il renderer fa una
// glDrawElementsInstanced per variante.
//
//   ./vat_horde [scena.scn]            (default: scenes/obstacles.scn)
// Camera mouse: PLAY = LMB drag pan / RMB drag rotate / rotellina zoom;
//            EDIT = Alt+LMB pan / Alt+RMB rotate / rotellina zoom (LMB/RMB nudi
//            = strumenti). Tastiera (sempre): frecce=pan/rotate  +/-=zoom.
// Controlli PLAY: C=camera  T=texture  SPACE=pausa  E=esplosione (lancio al
//            centro camera)  B=cassonetto (Shift=pesante)  N=auto (2 dischi+rod,
//            Shift=pesante)  F11=fullscreen  TAB=modalità EDIT  ESC=esci
// EDITOR (EDITOR_DESIGN fase 1, VAT_HORDE_EDIT=1 per partire in EDIT): la sim si
//   ferma, si edita la Scene e si re-instanzia tornando in PLAY (TAB). Tool a
//   tastiera: 1=select 2=goal 3=spawn 4=cost 5=pack 6=muro 7=costo-poly 8=prop.
//   Mouse: LMB drag = rect (goal/spawn/cost/pack); LMB click = vertice poligono
//   (o piazza un prop col tool 8)
//   (Invio chiude, Backspace/RMB toglie l'ultimo); RMB su un'entità = cancella.
//   G=snap on/off  [ ]=altezza poly / peso costo / TIPO prop  ,.=ruota prop
//   F2=salva la .scn caricata.
// Headless:  VAT_HORDE_SHOT="<frames>" ./vat_horde  -> simula N step, screenshot
//            vat_horde_shot.bmp, esce. VAT_HORDE_CAM="cx,cz,hh,az,el".
//            VAT_HORDE_BLAST="frame,x,y[,str,up]" -> esplosione+lancio a quel frame.
//            VAT_HORDE_BARRICADE="x,y,len[,mass]" / VAT_HORDE_CAR="x,y[,len][,mass]".
#include <SDL3/SDL.h>
#include "vat_gl.h"
#include "vat_layer.h"
#include "sim_particles.h"
#include "fx_particles.h"
#include "scene.h"
#include "defense.h"
#include "cgltf.h"
#include "terrain.h"
#include "props.h"
#include "destruct.h"
#include "edit_pick.h"
#include "editor.h"

#define MAXA 60000
#define NT 10                  // numero massimo di torrette (anello demo)
static float inst[MAXA*12];
static float shad[MAXA*4];     // ground shadow instances: xyz center + radius
#define EDOVL_MAXV 8192        // vertici max dell'overlay editor (rects+poly+cursore)
static float edovl[EDOVL_MAXV*9];

// I body type disponibili (asset bakati in vat/assets/). Texture placeholder: la
// skirt non ha ancora il _diffuse.png -> rende flat-shaded (tintata), corretto.
// Gli ULTIMI due (crawler, tank) sono body "di gioco": non assegnati a caso, solo
// via pin/set_variant. Il tank riusa la diffuse di zombie_man (stesso UVMap).
static const char *PREFIX[] = {
    "vat/assets/zombie_man", "vat/assets/zombie_man_obese",
    "vat/assets/zombie_fem", "vat/assets/zombie_fem_obese",
    "vat/assets/zombie_child", "vat/assets/zombie_fem_skirt",
    "vat/assets/zombie_maimed_arm",    /* monco di un braccio: tipo di gioco, non cosmetico */
    "vat/assets/zombie_maimed_legs",   /* crawler: tipo di gioco, non cosmetico */
    "vat/assets/zombie_tank",          /* tank: tipo di gioco, non cosmetico */
};
#define NVAR ((int)(sizeof(PREFIX)/sizeof(PREFIX[0])))
#define TANK_VAR    (NVAR-1)               /* indice del body tank */
#define CRAWLER_VAR (NVAR-2)               /* indice del body crawler */
#define ARM_VAR     (NVAR-3)               /* indice del body monco di un braccio */
#define NCOSMETIC   (NVAR-3)               /* le prime = body random cosmetici */

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
        case BT_TANK:return TANK_VAR; default:return 0; }   /* tank = modello proprio */
}
static void spawn_enemy(DefGame *g, SimP *s, VatLayer *vl, float x, float y, unsigned r){
    DefBody b=roll_body(r);
    SimPHandle h=def_spawn(g,x,y,b);
    if(h==SIMP_HANDLE_INVALID)return;
    int i=simp_index_of(s,h); int slot=simp_slot_of(s,i);
    vat_layer_pin_variant(vl,slot,body_variant(b,r>>7));
}

// §8: il director sceglie posizione+tipo (mix che si indurisce per ondata); qui
// agganciamo solo la variante VAT cosmetica all'agente appena nato.
// terreno render-only (EDITOR_DESIGN §9): quota per posare sprite/strutture; zero
// effetto sim. Dichiarato qui perché lo usano i callback eventi (sangue a terra).
static Terrain gTer; static int gTerOn = 0;
static float ter_z(float x, float y){ return gTerOn ? terrain_z(&gTer, x, y) : 0.0f; }
// ground per il particle system (gocce di sangue): coord orizzontali (x,z)=(x,y).
static float fx_ground(float x, float z, void *ud){ (void)ud; return ter_z(x,z); }

// schizzo sangue (FX_LAB step 2, identico a fxlab): burst fine di gocce rosso scuro
// con gravità che arcano e si posano (ground_stop). BURST = versione abbondante
// per la morte esplosiva.
static const FxEmitterDef BLOOD_DEF = {
    .count=28, .shape=FX_EMIT_POINT,
    .spawn_radius=0.05f, .spawn_box_z=0.05f,
    .spawn_offset_y_min=-0.05f, .spawn_offset_y_max=0.10f,
    .speed_xy_min=1.2f, .speed_xy_max=4.0f, .speed_y_min=1.2f, .speed_y_max=4.5f,
    .gravity=9.81f, .drag=0.6f, .lifetime_min=0.5f, .lifetime_max=1.2f,
    .start_scale_min=0.05f, .start_scale_max=0.11f, .end_scale_min=0.02f, .end_scale_max=0.05f,
    .start_color={0.50f,0.02f,0.02f,1.0f}, .end_color={0.22f,0.0f,0.0f,0.55f},
    .color_variants={ {0.55f,0.03f,0.03f,1.0f},{0.42f,0.0f,0.0f,1.0f},{0.60f,0.08f,0.04f,1.0f} },
    .color_variant_count=3, .sprite_first=-1, .sprite_last=-1,
    .wind_scale=0.3f, .ground_stop=true, .blend=FX_BLEND_ALPHA, .rate=20.0f,
};
static const FxEmitterDef BLOOD_BURST_DEF = {
    .count=90, .shape=FX_EMIT_POINT,
    .spawn_radius=0.10f, .spawn_box_z=0.10f,
    .spawn_offset_y_min=-0.05f, .spawn_offset_y_max=0.20f,
    .speed_xy_min=1.5f, .speed_xy_max=6.0f, .speed_y_min=2.0f, .speed_y_max=7.0f,
    .gravity=9.81f, .drag=0.55f, .lifetime_min=0.6f, .lifetime_max=1.6f,
    .start_scale_min=0.06f, .start_scale_max=0.16f, .end_scale_min=0.02f, .end_scale_max=0.06f,
    .start_color={0.50f,0.02f,0.02f,1.0f}, .end_color={0.20f,0.0f,0.0f,0.5f},
    .color_variants={ {0.55f,0.03f,0.03f,1.0f},{0.42f,0.0f,0.0f,1.0f},{0.60f,0.08f,0.04f,1.0f} },
    .color_variant_count=3, .sprite_first=-1, .sprite_last=-1,
    .wind_scale=0.3f, .ground_stop=true, .blend=FX_BLEND_ALPHA, .rate=20.0f,
};

// detriti dei prop distruttibili (DESTRUCT_DESIGN.md §5): schegge che volano via
// nella direzione di spinta dell'orda. wood = tavolini/sedie; metal = cartelli/
// semafori (frammenti grigi + qualche scheggia di vetro ciano).
static const FxEmitterDef WOOD_DEBRIS_DEF = {
    .count=16, .shape=FX_EMIT_POINT,
    .spawn_radius=0.15f, .spawn_box_z=0.15f,
    .spawn_offset_y_min=-0.10f, .spawn_offset_y_max=0.30f,
    .speed_xy_min=1.5f, .speed_xy_max=4.5f, .speed_y_min=1.5f, .speed_y_max=4.0f,
    .gravity=9.81f, .drag=0.35f, .lifetime_min=0.6f, .lifetime_max=1.3f,
    .start_scale_min=0.05f, .start_scale_max=0.12f, .end_scale_min=0.03f, .end_scale_max=0.06f,
    .start_color={0.45f,0.30f,0.14f,1.0f}, .end_color={0.30f,0.20f,0.09f,0.5f},
    .color_variants={ {0.50f,0.34f,0.16f,1.0f},{0.38f,0.25f,0.12f,1.0f},{0.55f,0.40f,0.22f,1.0f} },
    .color_variant_count=3, .sprite_first=-1, .sprite_last=-1,
    .wind_scale=0.2f, .ground_stop=true, .blend=FX_BLEND_ALPHA, .rate=20.0f,
};
static const FxEmitterDef METAL_DEBRIS_DEF = {
    .count=22, .shape=FX_EMIT_POINT,
    .spawn_radius=0.18f, .spawn_box_z=0.18f,
    .spawn_offset_y_min=-0.10f, .spawn_offset_y_max=0.60f,
    .speed_xy_min=2.0f, .speed_xy_max=6.0f, .speed_y_min=2.0f, .speed_y_max=5.5f,
    .gravity=9.81f, .drag=0.30f, .lifetime_min=0.6f, .lifetime_max=1.5f,
    .start_scale_min=0.05f, .start_scale_max=0.14f, .end_scale_min=0.02f, .end_scale_max=0.06f,
    .start_color={0.55f,0.57f,0.60f,1.0f}, .end_color={0.35f,0.37f,0.40f,0.5f},
    .color_variants={ {0.60f,0.62f,0.66f,1.0f},{0.45f,0.47f,0.50f,1.0f},
                      {0.55f,0.80f,0.85f,0.9f},{0.70f,0.90f,0.95f,0.85f} },  // 2 = vetro ciano
    .color_variant_count=4, .sprite_first=-1, .sprite_last=-1,
    .wind_scale=0.2f, .ground_stop=true, .blend=FX_BLEND_ALPHA, .rate=20.0f,
};

// scoppio di un prop distruttibile -> burst FX nel verso di spinta (cono ~35°).
typedef struct { FxParticles *fx; } DestructCtx;
static void on_prop_burst(int idx, const char *debris, float x, float y, float dir, void *ud){
    (void)idx;
    DestructCtx *c=(DestructCtx*)ud;
    int metal = (debris && (debris[0]=='m'||debris[0]=='g'));   // metal/glass vs wood
    const FxEmitterDef *def = metal ? &METAL_DEBRIS_DEF : &WOOD_DEBRIS_DEF;
    float o[3]={x, ter_z(x,y)+(metal?0.8f:0.4f), y};
    fx_emit(c->fx, o, def, dir, 0.6f);                          // half_angle ~35°
}

typedef struct { SimP *s; VatLayer *vl; FxParticles *fx; } SpawnCtx;
static void on_director_spawn(void *user, SimPHandle h, DefBody body, unsigned roll){
    SpawnCtx *c=(SpawnCtx*)user;
    int i=simp_index_of(c->s,h); if(i<0) return;
    vat_layer_pin_variant(c->vl, simp_slot_of(c->s,i), body_variant(body, roll>>7));
}

// hook eventi defense → effetti one-shot del render layer (gli stessi testati in
// fxlab): HIT = flinch + schizzo di sangue; DEATH = decedente (clip morte) → poi
// cadavere; GIB = morte esplosiva (burst grande + schizzo abbondante); WOUND_ARM/
// LEGS = mutilazione con mesh-gib 3D (arto reciso + frammenti) + sangue. Il cambio
// body (ARM/CRAWLER) e l'outfit insanguinato li applica il poll su def_wound nel
// loop (qui solo gli one-shot). L'insanguinamento al colpo lo fa il poll DW_BLOODY.
static void on_def_event(void *user, int slot, int i, DefBody body, DefEvent ev){
    SpawnCtx *c=(SpawnCtx*)user; (void)body;
    float x=simp_px(c->s)[i], y=simp_py(c->s)[i], r=simp_radius_arr(c->s)[i];
    unsigned seed=simp_handle_of(c->s,i);
    float hd=atan2f(simp_vy(c->s)[i], simp_vx(c->s)[i]);   // direzione di marcia → lato del taglio
    float o[3]={x, ter_z(x,y)+1.0f, y};                    // origine schizzo (~torace)
    if(ev==DEF_EV_HIT){ float dur=vat_layer_hit(c->vl, slot);   /* flinch + plant l'agente */
        if(dur>0.0f) simp_stun(c->s, i, dur);                  /* niente foot-sliding sotto la hit */
        fx_emit(c->fx, o, &BLOOD_DEF, 0.0f, -1.0f); }          /* schizzo sangue */
    else if(ev==DEF_EV_DEATH)
        vat_layer_die(c->vl, slot, x, y, r);
    else if(ev==DEF_EV_GIB){
        vat_layer_gib(c->vl, slot, x, y, r, seed);
        fx_emit(c->fx, o, &BLOOD_BURST_DEF, 0.0f, -1.0f); }
    else if(ev==DEF_EV_WOUND_BLEED){
        vat_layer_gib_wound(c->vl, slot, x, y, r, seed);
        fx_emit(c->fx, o, &BLOOD_DEF, 0.0f, -1.0f); }
    else if(ev==DEF_EV_WOUND_ARM){                              /* braccio reciso (mesh-gib) + sangue */
        vat_layer_maim_arm(c->vl, slot, x, y, r, hd, seed^0x1u);
        fx_emit(c->fx, o, &BLOOD_DEF, 0.0f, -1.0f); }
    else if(ev==DEF_EV_WOUND_LEGS){                             /* gambe recise (mesh-gib) + sangue */
        vat_layer_maim_legs(c->vl, slot, x, y, r, hd, seed^0x2u);
        float ol[3]={x, ter_z(x,y)+0.6f, y};                   // schizzo più basso (anca)
        fx_emit(c->fx, ol, &BLOOD_DEF, 0.0f, -1.0f); }
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

// --- gore mesh-gibs (FX_LAB): mesh di blend/gibs.glb (arm/frammenti/gambe) in un
// VAO ciascuna, CENTRATE sul centroide (il tumble ruota attorno al centro).
// mesh_id = ordine di nodo. UV sul diffuse del corpo. (copia di fxlab.load_gib_meshes)
typedef struct { GLuint vao; int nidx; } GibMesh;
static int load_gib_meshes(const char *path, GibMesh *out, int maxn){
    cgltf_options opt={0}; cgltf_data *data=NULL;
    if(cgltf_parse_file(&opt,path,&data)!=cgltf_result_success){ fprintf(stderr,"gibs: parse fail %s\n",path); return 0; }
    if(cgltf_load_buffers(&opt,data,path)!=cgltf_result_success){ fprintf(stderr,"gibs: load buffers fail\n"); cgltf_free(data); return 0; }
    int got=0;
    for(size_t n=0;n<data->nodes_count && got<maxn;n++){
        cgltf_node *nd=&data->nodes[n]; if(!nd->mesh||nd->mesh->primitives_count<1) continue;
        cgltf_primitive *pr=&nd->mesh->primitives[0];
        if(pr->type!=cgltf_primitive_type_triangles||!pr->indices) continue;
        cgltf_accessor *pos=NULL,*nrm=NULL,*uv=NULL;
        for(size_t a=0;a<pr->attributes_count;a++){ cgltf_attribute *at=&pr->attributes[a];
            if(at->type==cgltf_attribute_type_position) pos=at->data;
            else if(at->type==cgltf_attribute_type_normal) nrm=at->data;
            else if(at->type==cgltf_attribute_type_texcoord && at->index==0) uv=at->data; }
        if(!pos) continue;
        float M[16]; cgltf_node_transform_world(nd,M);
        size_t nv=pos->count, ni=pr->indices->count;
        float *verts=malloc(nv*8*sizeof(float)); unsigned short *idx=malloc(ni*sizeof(unsigned short));
        float cmin[3]={1e30f,1e30f,1e30f}, cmax[3]={-1e30f,-1e30f,-1e30f};
        for(size_t v=0;v<nv;v++){ float P[3]={0,0,0}; cgltf_accessor_read_float(pos,v,P,3);
            float wx=M[0]*P[0]+M[4]*P[1]+M[8]*P[2]+M[12];
            float wy=M[1]*P[0]+M[5]*P[1]+M[9]*P[2]+M[13];
            float wz=M[2]*P[0]+M[6]*P[1]+M[10]*P[2]+M[14];
            float *o=verts+v*8; o[0]=wx; o[1]=wy; o[2]=wz;
            if(wx<cmin[0])cmin[0]=wx; if(wx>cmax[0])cmax[0]=wx;
            if(wy<cmin[1])cmin[1]=wy; if(wy>cmax[1])cmax[1]=wy;
            if(wz<cmin[2])cmin[2]=wz; if(wz>cmax[2])cmax[2]=wz; }
        float ctr[3]={(cmin[0]+cmax[0])*0.5f,(cmin[1]+cmax[1])*0.5f,(cmin[2]+cmax[2])*0.5f};
        for(size_t v=0;v<nv;v++){ float *o=verts+v*8; o[0]-=ctr[0]; o[1]-=ctr[1]; o[2]-=ctr[2];
            float N[3]={0,1,0},T[2]={0,0};
            if(nrm) cgltf_accessor_read_float(nrm,v,N,3);
            if(uv)  cgltf_accessor_read_float(uv,v,T,2);
            o[3]=M[0]*N[0]+M[4]*N[1]+M[8]*N[2]; o[4]=M[1]*N[0]+M[5]*N[1]+M[9]*N[2];
            o[5]=M[2]*N[0]+M[6]*N[1]+M[10]*N[2]; o[6]=T[0]; o[7]=T[1]; }
        for(size_t k=0;k<ni;k++) idx[k]=(unsigned short)cgltf_accessor_read_index(pr->indices,k);
        GLuint vao,vbo,ebo; glGenVertexArrays(1,&vao);glBindVertexArray(vao);
        glGenBuffers(1,&vbo);glBindBuffer(GL_ARRAY_BUFFER,vbo);
        glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)nv*8*sizeof(float),verts,GL_STATIC_DRAW);
        glVertexAttribPointer(0,3,GL_FLOAT,0,8*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
        glVertexAttribPointer(1,3,GL_FLOAT,0,8*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
        glVertexAttribPointer(2,2,GL_FLOAT,0,8*sizeof(float),(void*)(6*sizeof(float)));glEnableVertexAttribArray(2);
        glGenBuffers(1,&ebo);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)ni*sizeof(unsigned short),idx,GL_STATIC_DRAW);
        glBindVertexArray(0); free(verts); free(idx);
        out[got].vao=vao; out[got].nidx=(int)ni; got++;
    }
    cgltf_free(data); return got;
}
// model matrix: T(world) * rot(axis,angle) * uniform scale (copia di fxlab.gib_model)
static void gib_model(mat4 m, float tx,float ty,float tz,
                      float ax,float ay,float az, float ang, float sc){
    float c=cosf(ang), s=sinf(ang), C=1.0f-c;
    float R[9]={ ax*ax*C+c,    ax*ay*C-az*s, ax*az*C+ay*s,
                 ay*ax*C+az*s, ay*ay*C+c,    ay*az*C-ax*s,
                 az*ax*C-ay*s, az*ay*C+ax*s, az*az*C+c };
    m[0]=R[0]*sc; m[1]=R[3]*sc; m[2]=R[6]*sc; m[3]=0.0f;
    m[4]=R[1]*sc; m[5]=R[4]*sc; m[6]=R[7]*sc; m[7]=0.0f;
    m[8]=R[2]*sc; m[9]=R[5]*sc; m[10]=R[8]*sc; m[11]=0.0f;
    m[12]=tx; m[13]=ty; m[14]=tz; m[15]=1.0f;
}
#define GIB_UNIT 0.01f   // unità Blender -> metri (braccio ~0.53 m); = fxlab
// veto editor (§10): non si piazza nulla su una cella-statico (buco palazzo).
static int ter_blocked(float x, float y){ return gTerOn && terrain_hole(&gTer, x, y); }

// catalogo prop di decoro (§10 stadio 5b): tipo->mesh+scala+label, render-only.
static PropCatalog gCatalog; static int gCatN = 0;

typedef struct { GLuint vao, vbo, ebo, tex; int nidx, hasTex; } Ground;

// carica la base-color texture del primo materiale (uri su file o embedded nel
// buffer view del .glb) come GL texture.
static GLuint ground_load_tex(cgltf_data *data, cgltf_primitive *prim, const char *dir){
    if(!prim->material || !prim->material->has_pbr_metallic_roughness) return 0;
    cgltf_texture *t = prim->material->pbr_metallic_roughness.base_color_texture.texture;
    if(!t || !t->image) return 0;
    cgltf_image *img = t->image;
    if(img->uri && strncmp(img->uri,"data:",5)!=0){            // external file
        char path[512]; snprintf(path,sizeof path,"%s%s",dir,img->uri);
        return vg_tex_png(path);
    }
    if(img->buffer_view){                                       // embedded image
        cgltf_buffer_view *bv = img->buffer_view;
        const unsigned char *base = (const unsigned char*)bv->buffer->data;
        int w,h,n; unsigned char *d = stbi_load_from_memory(base+bv->offset,(int)bv->size,&w,&h,&n,0);
        if(!d){ printf("ground: embedded image decode fail\n"); return 0; }
        GLuint tex; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex);
        GLenum fmt=(n==4)?GL_RGBA:GL_RGB;
        glTexImage2D(GL_TEXTURE_2D,0,fmt,w,h,0,fmt,GL_UNSIGNED_BYTE,d); glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        stbi_image_free(d); printf("ground: embedded texture %dx%d\n",w,h);
        return tex;
    }
    return 0;
}

// carica la mesh del suolo da .glb: accumula TUTTE le primitive di TUTTI i nodi
// con la matrice mondo del nodo. glTF è Y-up: mappa al nostro mondo (x, y, -z),
// coerente col bake (terrain_bake.py legge gli assi di Blender). 8 float/vertice
// (pos.xyz, normal.xyz, uv.xy). Ritorna 0 su successo.
static int load_ground_glb(const char *path, Ground *G){
    memset(G,0,sizeof *G);
    cgltf_options opt={0}; cgltf_data *data=NULL;
    if(cgltf_parse_file(&opt,path,&data)!=cgltf_result_success){ fprintf(stderr,"ground: parse fail %s\n",path); return -1; }
    if(cgltf_load_buffers(&opt,data,path)!=cgltf_result_success){ fprintf(stderr,"ground: load buffers fail\n"); cgltf_free(data); return -1; }

    // dir del glb (per le texture esterne relative)
    char dir[512]; snprintf(dir,sizeof dir,"%s",path);
    char *slash=strrchr(dir,'/'); if(slash) slash[1]='\0'; else dir[0]='\0';

    // pass 1: conta vertici/indici totali (solo nodi con mesh)
    size_t totV=0, totI=0;
    for(size_t n=0;n<data->nodes_count;n++){ cgltf_node *nd=&data->nodes[n]; if(!nd->mesh) continue;
        for(size_t p=0;p<nd->mesh->primitives_count;p++){ cgltf_primitive *pr=&nd->mesh->primitives[p];
            if(pr->type!=cgltf_primitive_type_triangles||!pr->indices) continue;
            cgltf_accessor *pos=NULL; for(size_t a=0;a<pr->attributes_count;a++) if(pr->attributes[a].type==cgltf_attribute_type_position) pos=pr->attributes[a].data;
            if(!pos) continue; totV+=pos->count; totI+=pr->indices->count; } }
    if(totV==0||totI==0){ fprintf(stderr,"ground: no triangles in %s\n",path); cgltf_free(data); return -1; }

    float *verts=malloc(totV*8*sizeof(float)); unsigned *idx=malloc(totI*sizeof(unsigned));
    GLuint tex=0;
    size_t vbase=0, ibase=0;
    for(size_t n=0;n<data->nodes_count;n++){ cgltf_node *nd=&data->nodes[n]; if(!nd->mesh) continue;
        float M[16]; cgltf_node_transform_world(nd,M);
        for(size_t p=0;p<nd->mesh->primitives_count;p++){ cgltf_primitive *pr=&nd->mesh->primitives[p];
            if(pr->type!=cgltf_primitive_type_triangles||!pr->indices) continue;
            cgltf_accessor *pos=NULL,*nrm=NULL,*uv=NULL;
            for(size_t a=0;a<pr->attributes_count;a++){ cgltf_attribute *at=&pr->attributes[a];
                if(at->type==cgltf_attribute_type_position) pos=at->data;
                else if(at->type==cgltf_attribute_type_normal) nrm=at->data;
                else if(at->type==cgltf_attribute_type_texcoord && at->index==0) uv=at->data; }
            if(!pos) continue;
            if(!tex) tex=ground_load_tex(data,pr,dir);
            for(size_t v=0;v<pos->count;v++){
                float P[3]={0,0,0},N[3]={0,1,0},T[2]={0,0};
                cgltf_accessor_read_float(pos,v,P,3);
                if(nrm) cgltf_accessor_read_float(nrm,v,N,3);
                if(uv)  cgltf_accessor_read_float(uv,v,T,2);
                // node world transform (column-major), then glTF y-up -> world (x,y,-z)
                float wx=M[0]*P[0]+M[4]*P[1]+M[8]*P[2]+M[12];
                float wy=M[1]*P[0]+M[5]*P[1]+M[9]*P[2]+M[13];
                float wz=M[2]*P[0]+M[6]*P[1]+M[10]*P[2]+M[14];
                float nx=M[0]*N[0]+M[4]*N[1]+M[8]*N[2];
                float ny=M[1]*N[0]+M[5]*N[1]+M[9]*N[2];
                float nz=M[2]*N[0]+M[6]*N[1]+M[10]*N[2];
                float *o=verts+(vbase+v)*8;
                o[0]=wx; o[1]=wy; o[2]=-wz; o[3]=nx; o[4]=ny; o[5]=-nz; o[6]=T[0]; o[7]=T[1];
            }
            for(size_t k=0;k<pr->indices->count;k++)
                idx[ibase+k]=(unsigned)(vbase+cgltf_accessor_read_index(pr->indices,k));
            vbase+=pos->count; ibase+=pr->indices->count;
        } }
    cgltf_free(data);

    glGenVertexArrays(1,&G->vao); glBindVertexArray(G->vao);
    glGenBuffers(1,&G->vbo); glBindBuffer(GL_ARRAY_BUFFER,G->vbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)vbase*8*sizeof(float),verts,GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,8*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,0,8*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,2,GL_FLOAT,0,8*sizeof(float),(void*)(6*sizeof(float)));glEnableVertexAttribArray(2);
    glGenBuffers(1,&G->ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,G->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)ibase*sizeof(unsigned),idx,GL_STATIC_DRAW);
    glBindVertexArray(0);
    G->nidx=(int)ibase; G->tex=tex; G->hasTex=(tex!=0);
    free(verts); free(idx);
    printf("ground: %s  %zu verts %zu tris  tex=%d\n",path,vbase,ibase/3,G->hasTex);
    return 0;
}

// --- mesh statica degli ostacoli: ogni poligono estruso (top + pareti) + suolo.
// 9 float/vertice: pos.xyz, normal.xyz, color.rgb. World = (sim_x, altezza, sim_y).
static float *build_obstacle_mesh(const Scene *sc, int with_ground, int *out_nverts) {
    int nv = with_ground ? 6 : 0;                // ground quad (skipped if terrain mesh)
    for (int k = 0; k < sc->n_poly; k++) { int n = sc->poly[k].nverts; nv += (n - 2) * 3 + n * 6; }
    float *buf = malloc((size_t)nv * 9 * sizeof(float));
    int c = 0;
#define PUSH(PX,PY,PZ,NX,NY,NZ,R,G,B) do{ float*o=buf+c*9; \
    o[0]=(PX);o[1]=(PY);o[2]=(PZ); o[3]=(NX);o[4]=(NY);o[5]=(NZ); o[6]=(R);o[7]=(G);o[8]=(B); c++; }while(0)
    if (with_ground) {  // suolo flat (leggermente sotto 0); col terreno glb lo salta
        float W = sc->world_w, H = sc->world_h, gy = -0.02f;
        PUSH(0,gy,0, 0,1,0, 0.16f,0.18f,0.15f); PUSH(W,gy,0, 0,1,0, 0.16f,0.18f,0.15f); PUSH(W,gy,H, 0,1,0, 0.16f,0.18f,0.15f);
        PUSH(0,gy,0, 0,1,0, 0.16f,0.18f,0.15f); PUSH(W,gy,H, 0,1,0, 0.16f,0.18f,0.15f); PUSH(0,gy,H, 0,1,0, 0.16f,0.18f,0.15f);
    }

    for (int k = 0; k < sc->n_poly; k++) {
        const ScenePoly *p = &sc->poly[k];
        int n = p->nverts;
        float cr = p->solid ? 0.56f : 0.46f, cg = p->solid ? 0.56f : 0.34f, cb = p->solid ? 0.60f : 0.20f;
        float ccx = 0, ccz = 0;
        for (int i = 0; i < n; i++) { ccx += p->vx[i]; ccz += p->vy[i]; }
        ccx /= n; ccz /= n;
        float zb = ter_z(ccx, ccz), h = zb + p->height;   // seat base/top on terrain
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
            PUSH(p->vx[k1],zb,p->vy[k1], nx,0,nz, wr,wg,wb);
            PUSH(p->vx[k0],h,p->vy[k0], nx,0,nz, wr,wg,wb);
            PUSH(p->vx[k1],zb,p->vy[k1], nx,0,nz, wr,wg,wb);
            PUSH(p->vx[k0],zb,p->vy[k0], nx,0,nz, wr,wg,wb);
        }
    }
#undef PUSH
    *out_nverts = c;
    return buf;
}

// --- mesh statica dei prop di decoro (§10 stadio 5b): render-only, NESSUN effetto
// sim. Finché non c'è l'arte (un .glb per tipo nel catalogo), ogni prop è un
// PLACEHOLDER procedurale: un corpo basso + un montante sottile su un lato, così
// la rotazione Y si legge. Seatato sul terreno (ter_z), scalato dal catalogo,
// colorato per tipo (slot del catalogo → palette; chiave sconosciuta = magenta).
// Stesso layout 9-float del flat shader (pos, normal, color), coord di mondo.
static const float PROP_PAL[8][3] = {
    {0.80f,0.55f,0.30f},{0.40f,0.62f,0.80f},{0.55f,0.75f,0.40f},{0.78f,0.72f,0.40f},
    {0.70f,0.45f,0.55f},{0.45f,0.70f,0.68f},{0.72f,0.58f,0.42f},{0.60f,0.55f,0.72f} };

// box ruotato attorno a (cx,cz): footprint [±hx,±hz] traslato di (ox,oz) nel
// frame locale, base zb, altezza hy, normali ruotate. Ritorna il nuovo conteggio.
static int prop_box(float *buf, int c, float cx, float cz, float ox, float oz,
                    float zb, float hx, float hz, float hy, float ca, float sa,
                    float r, float g, float b) {
    float lx[4]={ox-hx,ox+hx,ox+hx,ox-hx}, lz[4]={oz-hz,oz-hz,oz+hz,oz+hz};
    float wx[4],wz[4], bx=0,bz=0;
    for(int i=0;i<4;i++){ wx[i]=cx+lx[i]*ca-lz[i]*sa; wz[i]=cz+lx[i]*sa+lz[i]*ca; bx+=wx[i]; bz+=wz[i]; }
    bx*=0.25f; bz*=0.25f;
    float y0=zb, y1=zb+hy;
#define PP(X,Y,Z,NX,NY,NZ) do{ float*o=buf+c*9; o[0]=(X);o[1]=(Y);o[2]=(Z); \
    o[3]=(NX);o[4]=(NY);o[5]=(NZ); o[6]=r;o[7]=g;o[8]=b; c++; }while(0)
    PP(wx[0],y1,wz[0],0,1,0); PP(wx[1],y1,wz[1],0,1,0); PP(wx[2],y1,wz[2],0,1,0);
    PP(wx[0],y1,wz[0],0,1,0); PP(wx[2],y1,wz[2],0,1,0); PP(wx[3],y1,wz[3],0,1,0);
    for(int k=0;k<4;k++){ int k1=(k+1)&3;
        float ex=wx[k1]-wx[k], ez=wz[k1]-wz[k], nx=ez, nz=-ex;
        float l=sqrtf(nx*nx+nz*nz); if(l>1e-6f){ nx/=l; nz/=l; }
        float mx=(wx[k]+wx[k1])*0.5f-bx, mz=(wz[k]+wz[k1])*0.5f-bz;
        if(nx*mx+nz*mz<0){ nx=-nx; nz=-nz; }
        float s=0.85f, wr=r*s, wg=g*s, wb=b*s; (void)wr;(void)wg;(void)wb;
        PP(wx[k],y1,wz[k],nx,0,nz); PP(wx[k1],y1,wz[k1],nx,0,nz); PP(wx[k1],y0,wz[k1],nx,0,nz);
        PP(wx[k],y1,wz[k],nx,0,nz); PP(wx[k1],y0,wz[k1],nx,0,nz); PP(wx[k],y0,wz[k],nx,0,nz);
    }
#undef PP
    return c;
}

// box inclinabile: come prop_box ma la faccia superiore (a quota hy) e' spostata
// orizzontalmente di (tdx,tdz) e abbassata a topH -> rotazione rigida attorno
// alla base per l'abbattimento (DESTRUCT_DESIGN.md §6). tdx=tdz=0,topH=hy = dritto.
static int prop_box_lean(float *buf, int c, float cx, float cz, float ox, float oz,
                    float zb, float hx, float hz, float hy, float ca, float sa,
                    float r, float g, float b, float tdx, float tdz, float topH) {
    float lx[4]={ox-hx,ox+hx,ox+hx,ox-hx}, lz[4]={oz-hz,oz-hz,oz+hz,oz+hz};
    float wx[4],wz[4], bx=0,bz=0;
    for(int i=0;i<4;i++){ wx[i]=cx+lx[i]*ca-lz[i]*sa; wz[i]=cz+lx[i]*sa+lz[i]*ca; bx+=wx[i]; bz+=wz[i]; }
    bx*=0.25f; bz*=0.25f; (void)hy;
    float tx[4],tz[4]; for(int i=0;i<4;i++){ tx[i]=wx[i]+tdx; tz[i]=wz[i]+tdz; }
    float y0=zb, y1=zb+topH;
#define PP(X,Y,Z,NX,NY,NZ) do{ float*o=buf+c*9; o[0]=(X);o[1]=(Y);o[2]=(Z); \
    o[3]=(NX);o[4]=(NY);o[5]=(NZ); o[6]=r;o[7]=g;o[8]=b; c++; }while(0)
    PP(tx[0],y1,tz[0],0,1,0); PP(tx[1],y1,tz[1],0,1,0); PP(tx[2],y1,tz[2],0,1,0);
    PP(tx[0],y1,tz[0],0,1,0); PP(tx[2],y1,tz[2],0,1,0); PP(tx[3],y1,tz[3],0,1,0);
    for(int k=0;k<4;k++){ int k1=(k+1)&3;
        float ex=wx[k1]-wx[k], ez=wz[k1]-wz[k], nx=ez, nz=-ex;
        float l=sqrtf(nx*nx+nz*nz); if(l>1e-6f){ nx/=l; nz/=l; }
        float mx=(wx[k]+wx[k1])*0.5f-bx, mz=(wz[k]+wz[k1])*0.5f-bz;
        if(nx*mx+nz*mz<0){ nx=-nx; nz=-nz; }
        PP(tx[k],y1,tz[k],nx,0,nz); PP(tx[k1],y1,tz[k1],nx,0,nz); PP(wx[k1],y0,wz[k1],nx,0,nz);
        PP(tx[k],y1,tz[k],nx,0,nz); PP(wx[k1],y0,wz[k1],nx,0,nz); PP(wx[k],y0,wz[k],nx,0,nz);
    }
#undef PP
    return c;
}

#define PROP_VERTS_EACH 60   // corpo (30) + montante (30)
#define PROP_TOPPLE_MAX 1.40f  // ~80 deg di abbattimento a t=1
// build con stato di distruzione opzionale (NULL = tutti intatti, es. in EDIT).
static float *build_prop_mesh(const Scene *sc, const PropCatalog *cat,
                              const Destruct *dz, int *out_nv) {
    int nv = sc->n_prop * PROP_VERTS_EACH;
    float *buf = malloc((size_t)(nv>0?nv:1) * 9 * sizeof(float));
    int c = 0;
    for (int i = 0; i < sc->n_prop; i++) {
        int st = dz ? destruct_state(dz, i) : DESTRUCT_INERT;
        if (st == DESTRUCT_GONE) continue;                 // distrutto: sparito
        const SceneProp *pr = &sc->prop[i];
        const PropDef *d = prop_catalog_find(cat, pr->key);
        float sc_m = d ? d->scale : 1.0f;
        float r,g,b;
        if (d) { int idx = (int)(d - cat->defs) & 7; r=PROP_PAL[idx][0]; g=PROP_PAL[idx][1]; b=PROP_PAL[idx][2]; }
        else   { r=0.90f; g=0.10f; b=0.85f; }            // chiave sconosciuta = magenta
        float a = pr->rot * 0.01745329f, ca = cosf(a), sa = sinf(a);
        float zb = ter_z(pr->x, pr->y);
        // abbattimento: theta cresce 0..MAX, la faccia alta ruota attorno alla base
        float th = (st==DESTRUCT_TOPPLING) ? destruct_topple_t(dz,i)*PROP_TOPPLE_MAX : 0.0f;
        float st_=sinf(th), ct_=cosf(th);
        float dh = (st==DESTRUCT_TOPPLING) ? destruct_dir(dz,i) : 0.0f;
        float ddx=cosf(dh), ddz=sinf(dh);
        // corpo: 0.7×0.4 m, alto 0.45 m
        float bH=0.45f*sc_m;
        c = prop_box_lean(buf, c, pr->x, pr->y, 0,0, zb, 0.35f*sc_m, 0.20f*sc_m, bH, ca,sa, r,g,b,
                          bH*st_*ddx, bH*st_*ddz, bH*ct_);
        // montante sul lato frontale (+x locale): 0.1×0.1 m, alto 1.0 m
        float mH=1.0f*sc_m;
        c = prop_box_lean(buf, c, pr->x, pr->y, 0.30f*sc_m,0, zb, 0.06f*sc_m, 0.06f*sc_m, mH, ca,sa,
                          r*0.8f, g*0.8f, b*0.8f, mH*st_*ddx, mH*st_*ddz, mH*ct_);
    }
    *out_nv = c;
    return buf;
}

// ricarica la mesh prop nel VBO (i prop della Scene cambiano in EDIT / si distruggono).
static int upload_prop_mesh(GLuint vbo, const Scene *sc, const PropCatalog *cat,
                            const Destruct *dz){
    int nv=0; float *m=build_prop_mesh(sc,cat,dz,&nv);
    glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(nv>0?nv:1)*9*sizeof(float),m,GL_STATIC_DRAW);
    free(m); return nv;
}

// --- mesh dei draggable (DRAG_DESIGN.md): un cassonetto (box) per oggetto,
// orientato sulla velocità (così si LEGGE che scivola), seatato sul terreno.
// Rebuilt OGNI frame dalle posizioni della sim (gli oggetti si muovono). Stesso
// layout 9-float del flat shader. Restituisce il conteggio vertici.
#define DRAG_VERTS_EACH 30           // prop_box: top(6) + 4 pareti(24)
#define DRAG_DRAW_CAP   256          // cassonetti renderizzati al massimo
static int build_drag_mesh(SimP *s, float *buf){
    int ndtot=simp_drag_count(s);
    int nd=ndtot; if(nd>DRAG_DRAW_CAP) nd=DRAG_DRAW_CAP;
    const float *px=simp_drag_px(s),*py=simp_drag_py(s);
    const float *vx=simp_drag_vx(s),*vy=simp_drag_vy(s),*rad=simp_drag_rad(s);
    // auto = due dischi tenuti da un rod (DRAG_DESIGN.md §8): disegna UNA scatola
    // allungata orientata sul rod, e marca i due dischi per non ridisegnarli come
    // cassonetti singoli sotto. Letti freschi ogni frame (indici non stabili).
    static uint8_t is_car[DRAG_DRAW_CAP];
    for(int i=0;i<nd;i++) is_car[i]=0;
    int c=0, nl=simp_drag_link_count(s);
    for(int k=0;k<nl;k++){
        int a,b; if(!simp_drag_link_pair(s,k,&a,&b)) continue;
        if(a>=nd||b>=nd) continue;
        is_car[a]=is_car[b]=1;
        float mx=0.5f*(px[a]+px[b]), my=0.5f*(py[a]+py[b]);
        float dx=px[b]-px[a], dy=py[b]-py[a];
        float len=sqrtf(dx*dx+dy*dy);
        float ang=(len>1e-4f)?atan2f(dy,dx):0.0f, ca=cosf(ang),sa=sinf(ang);
        float r=0.5f*(rad[a]+rad[b]), zb=ter_z(mx,my);
        // corpo auto: lungo come il rod + sbalzo, largo ~raggio, basso e largo
        c=prop_box(buf,c, mx,my, 0,0, zb, len*0.5f+r, r*0.85f, r*1.6f, ca,sa,
                   0.62f,0.18f,0.16f);  // rosso scuro = vernice auto
    }
    // cassonetti singoli (dischi non legati a un'auto): acciaio sporco, su velocità
    for(int i=0;i<nd;i++){
        if(is_car[i]) continue;
        float r=rad[i];
        float a=(vx[i]*vx[i]+vy[i]*vy[i]>1e-4f)?atan2f(vy[i],vx[i]):0.0f;
        float ca=cosf(a),sa=sinf(a), zb=ter_z(px[i],py[i]);
        c=prop_box(buf,c, px[i],py[i], 0,0, zb, r*0.95f, r*0.78f, r*2.0f, ca,sa,
                   0.40f,0.43f,0.48f);
    }
    return c;
}

// piazza un'AUTO (DRAG_DESIGN.md §8): due dischi a distanza `len` lungo +x,
// legati da un rod rigido, massa `mass` ciascuno. Orientamento iniziale lungo x.
static void place_car(SimP *s, float x, float y, float len, float mass){
    float r=0.6f;
    int a=simp_drag_add(s, x-len*0.5f, y, r, mass);
    int b=simp_drag_add(s, x+len*0.5f, y, r, mass);
    if(a>=0&&b>=0) simp_drag_link(s,a,b);
}

// piazza una BARRICATA: fila verticale di dischi draggable accostati attorno a
// (x,y), lunga `len` m, ognuno massa `mass`. Per la verifica visiva (env/tasto).
static void place_barricade(SimP *s, float x, float y, float len, float mass){
    float r=0.6f, step=r*1.95f;                 // accostati (collidono tra loro)
    int n=(int)(len/step); if(n<1) n=1;
    float y0=y-len*0.5f;
    for(int k=0;k<=n;k++) simp_drag_add(s, x, y0+k*step, r, mass);
}

// --- mesh statica delle torrette: un pilastrino per torretta (arancio = leggera,
// rosso = pesante). Stesso layout 9-float del flat shader (pos, normal, color).
// Rebuilt each frame into a caller buffer (sized def_turret_count*30 verts): a
// turret sieged to collapse (def_turret_disabled) vanishes. Returns vertex count.
static int build_turret_mesh(DefGame *g, float *buf){
    int nt=def_turret_count(g); int c=0;
    for(int id=0;id<nt;id++){ DefTurret *t=def_turret(g,id);
        if(def_turret_disabled(g,id)) continue;            // destroyed: gone
        float cx=t->x, cz=t->y, hw=0.32f;
        float zb=ter_z(cx,cz), h=zb+1.6f;            // seat on terrain
        float cr=0.95f, cg=t->heavy?0.20f:0.55f, cb=0.10f;
        float x0=cx-hw,x1=cx+hw,z0=cz-hw,z1=cz+hw;
#define VT(PX,PY,PZ,NX,NY,NZ) do{float*o=buf+c*9;o[0]=PX;o[1]=PY;o[2]=PZ;\
        o[3]=NX;o[4]=NY;o[5]=NZ;o[6]=cr;o[7]=cg;o[8]=cb;c++;}while(0)
#define QT(ax,ay,az,bx,by,bz,px2,py2,pz2,dx,dy,dz,nx,ny,nz) do{ \
        VT(ax,ay,az,nx,ny,nz);VT(bx,by,bz,nx,ny,nz);VT(px2,py2,pz2,nx,ny,nz); \
        VT(ax,ay,az,nx,ny,nz);VT(px2,py2,pz2,nx,ny,nz);VT(dx,dy,dz,nx,ny,nz);}while(0)
        QT(x0,h,z0, x1,h,z0, x1,h,z1, x0,h,z1, 0,1,0);    // top
        QT(x1,zb,z0, x1,zb,z1, x1,h,z1, x1,h,z0, 1,0,0);    // +X
        QT(x0,zb,z1, x0,zb,z0, x0,h,z0, x0,h,z1, -1,0,0);   // -X
        QT(x0,zb,z1, x1,zb,z1, x1,h,z1, x0,h,z1, 0,0,1);    // +Z
        QT(x1,zb,z0, x0,zb,z0, x0,h,z0, x1,h,z0, 0,0,-1);   // -Z
#undef VT
#undef QT
    }
    return c;
}

// --- §7 structures & siege: groups of wall nav cells sharing one HP pool; the
// horde sieges them (def_update), HP drops, cells free on collapse. Two sources:
// the legacy demo base (VAT_HORDE_BASE, two concentric rings) and data-driven
// `wall` entries from the scene (the test maps). State file-scope so the
// per-frame mesh rebuild and HUD can read it. gStX0..gStY1 = bbox of ALL
// structure cells (mesh sweep); gCoreId >= 0 only for the legacy base.
static int gStructOn=0, gCoreId=-1, gOuterId=-1;
static int gStX0=0, gStY0=0, gStX1=-1, gStY1=-1;
static void build_base(DefGame *g, SimP *s, float cell, float bcx, float bcy){
    int cx0=(int)(bcx/cell), cy0=(int)(bcy/cell), hc=6, ho=16;
    gCoreId  = def_add_structure(g, 1200.0f, 1);   // innermost = loss
    gOuterId = def_add_structure(g,  800.0f, 0);   // reroutes on collapse
    for(int cy=cy0-hc;cy<=cy0+hc;cy++)for(int cx=cx0-hc;cx<=cx0+hc;cx++)
        if(cx==cx0-hc||cx==cx0+hc||cy==cy0-hc||cy==cy0+hc) def_struct_cell(g,gCoreId,cx,cy);
    for(int cy=cy0-ho;cy<=cy0+ho;cy++)for(int cx=cx0-ho;cx<=cx0+ho;cx++)
        if(cx==cx0-ho||cx==cx0+ho||cy==cy0-ho||cy==cy0+ho) def_struct_cell(g,gOuterId,cx,cy);
    simp_terrain_commit(s);
    gStX0=cx0-ho; gStY0=cy0-ho; gStX1=cx0+ho; gStY1=cy0+ho; gStructOn=1;
}
// data-driven destructible walls from the scene (§7 test maps): one structure
// per `wall` rect, HP = wall.hp, per-cell breakthrough cost = cost_mult x base
// (a BREACH is a gap between segments; a WEAK section a low hp + low mult one).
static void build_walls_from_scene(DefGame *g, SimP *s, const Scene *sc){
    int gw=simp_grid_w(s), gh=simp_grid_h(s);
    float base=simp_wall_base_cost();
    // Hybrid (DRAG_DESIGN.md): VAT_HORDE_DEBRIS=mass -> i muri della scena al
    // crollo si sbriciolano in detriti draggable che l'orda shova fuori dalla breccia.
    float debris=getenv("VAT_HORDE_DEBRIS")?atof(getenv("VAT_HORDE_DEBRIS")):0.0f;
    int x0=gw, y0=gh, x1=-1, y1=-1;
    for(int k=0;k<sc->n_wall;k++){
        const SceneWall *w=&sc->wall[k];
        int id=def_add_structure(g, w->hp, 0);
        if(debris>0.0f) def_struct_set_debris(g,id,debris);
        int cx0=(int)floorf(w->x/sc->cell), cy0=(int)floorf(w->y/sc->cell);
        int cx1=(int)floorf((w->x+w->w)/sc->cell-1e-4f), cy1=(int)floorf((w->y+w->h)/sc->cell-1e-4f);
        if(cx0<0)cx0=0; if(cy0<0)cy0=0; if(cx1>=gw)cx1=gw-1; if(cy1>=gh)cy1=gh-1;
        for(int cy=cy0;cy<=cy1;cy++)for(int cx=cx0;cx<=cx1;cx++){
            def_struct_cell(g,id,cx,cy);                    // raises wall + barricade cost
            simp_set_wall_cost(s,cx,cy, w->cost_mult*base); // override tier (forte/debole)
            if(cx<x0)x0=cx; if(cy<y0)y0=cy; if(cx>x1)x1=cx; if(cy>y1)y1=cy;
        }
    }
    simp_terrain_commit(s);
    if(x1>=x0){ gStX0=x0; gStY0=y0; gStX1=x1; gStY1=y1; gStructOn=1; }
}
// rebuild the live structure mesh each frame from def_cell_struct: collapsed
// cells vanish, surviving cells darken as their structure's HP drops. A box per
// live cell, 9-float flat layout. Returns vertex count.
static int build_struct_mesh(DefGame *g, float cell, float *buf){
    int c=0; float H=2.8f;
    for(int cy=gStY0-1; cy<=gStY1+1; cy++)
    for(int cx=gStX0-1; cx<=gStX1+1; cx++){
        int id=def_cell_struct(g,cx,cy); if(id<0) continue;
        if(def_struct_is_turret(g,id)) continue;          // drawn as a turret pillar
        float frac=def_struct_hp(g,id)/ (def_struct_hp_max(g,id)+1e-3f); // 1..0
        float t=0.35f+0.65f*frac;                         // darken with damage
        float cr,cg,cb;
        if(id==gCoreId){ cr=0.75f*t; cg=0.16f*t; cb=0.16f*t; }   // core = red
        else           { cr=0.55f*t; cg=0.57f*t; cb=0.62f*t; }   // ring = steel
        float x0=cx*cell, x1=x0+cell, z0=cy*cell, z1=z0+cell;
        float zb=ter_z((x0+x1)*0.5f,(z0+z1)*0.5f), H1=zb+H;   // seat on terrain
#define VS(PX,PY,PZ,NX,NY,NZ) do{float*o=buf+c*9;o[0]=PX;o[1]=PY;o[2]=PZ;\
        o[3]=NX;o[4]=NY;o[5]=NZ;o[6]=cr;o[7]=cg;o[8]=cb;c++;}while(0)
#define QS(ax,ay,az,bx,by,bz,px2,py2,pz2,dx,dy,dz,nx,ny,nz) do{ \
        VS(ax,ay,az,nx,ny,nz);VS(bx,by,bz,nx,ny,nz);VS(px2,py2,pz2,nx,ny,nz); \
        VS(ax,ay,az,nx,ny,nz);VS(px2,py2,pz2,nx,ny,nz);VS(dx,dy,dz,nx,ny,nz);}while(0)
        QS(x0,H1,z0, x1,H1,z0, x1,H1,z1, x0,H1,z1, 0,1,0);    // top
        QS(x1,zb,z0, x1,zb,z1, x1,H1,z1, x1,H1,z0, 1,0,0);    // +X
        QS(x0,zb,z1, x0,zb,z0, x0,H1,z0, x0,H1,z1, -1,0,0);   // -X
        QS(x0,zb,z1, x1,zb,z1, x1,H1,z1, x0,H1,z1, 0,0,1);    // +Z
        QS(x1,zb,z0, x0,zb,z0, x0,H1,z0, x1,H1,z0, 0,0,-1);   // -Z
#undef VS
#undef QS
    }
    return c;
}

// Costruisce il mondo VIVO (sim + gameplay) DERIVATO dalla Scene (EDITOR_DESIGN
// §1): scene_instantiate + torrette ad anello + base opz. + prefill bench +
// director. Chiamato all'avvio e di nuovo su EDIT→PLAY (re-instantiate). Aggiorna
// spctx->s per il callback del director. Ritorna 0, -1 su fallimento.
// Statici indistruttibili dal terreno: ogni cella-buco della maschera ZHM2
// (EDITOR_DESIGN §9) diventa un muro PERMANENTE col tier 'palazzo' (alto, ≫
// barricata) del costo di sfondamento per-cella → l'orda li aggira invece di
// premerli. La collisione SDF è quella standard del muro.
#define PALAZZO_WALL_MULT 10.0f
typedef struct { SimP *s; int n; } HoleCtx;
static void hole_wall_cb(void *u, int cx, int cy){
    HoleCtx *c = (HoleCtx*)u;
    simp_set_wall(c->s, cx, cy, true);
    simp_set_wall_cost(c->s, cx, cy, PALAZZO_WALL_MULT * simp_wall_base_cost());
    c->n++;
}

static int build_world(const Scene *sc, VatLayer *vl, int fillN, SpawnCtx *spctx,
                       SimP **ps, DefGame **pg, DefDirector **pdir){
    SimP *s = scene_instantiate(sc, MAXA);
    if(!s){ fprintf(stderr,"scene_instantiate fail\n"); return -1; }
    spctx->s = s; spctx->vl = vl;

    // buchi del terreno (statici indistruttibili) PRIMA di prefill/base, così
    // gli agenti non nascono dentro un palazzo e il nav è già corretto.
    if(gTerOn){ HoleCtx hc={s,0};
        terrain_each_hole_cell(&gTer, sc->cell, simp_grid_w(s), simp_grid_h(s), hole_wall_cb, &hc);
        if(hc.n){ simp_terrain_commit(s);
            printf("statici terreno: %d celle-muro (tier palazzo) dai buchi ZHM2\n", hc.n); } }

    DefGame *g = def_create(s, MAXA);
    def_set_event_cb(g, on_def_event, spctx);   // hit/death -> animazioni one-shot
    float bcx=sc->world_w*0.5f, bcy=sc->world_h*0.5f;
    if(sc->n_goal>0){ float sx=0,sy=0; for(int k=0;k<sc->n_goal;k++){
            sx+=sc->goal[k].x+sc->goal[k].w*0.5f; sy+=sc->goal[k].y+sc->goal[k].h*0.5f; }
        bcx=sx/sc->n_goal; bcy=sy/sc->n_goal; }
    float mn = sc->world_w<sc->world_h?sc->world_w:sc->world_h;
    float TR_R = 0.22f*mn;
    def_set_budget(g, getenv("VAT_HORDE_BUDGET")?atoi(getenv("VAT_HORDE_BUDGET")):1000);
    int placed=0;
    // a "designed" scene (walls and/or turrets authored) owns its turrets: place
    // ONLY the scene's (possibly zero). A legacy scene gets the demo auto-ring.
    int designed = (sc->n_wall>0 || sc->n_turret>0);
    // destructible turrets: a turret becomes a 1-cell solid the horde sieges to
    // reach the goal beyond -> exposed turrets in a breached ring get assaulted
    // and silenced (def_turret_make_destructible). HP from env, 0 = indestructible.
    float turret_hp = getenv("VAT_HORDE_TURRET_HP")?atof(getenv("VAT_HORDE_TURRET_HP")):250.0f;
    // contact-siege tuning (def_set_turret_contact): 0 = keep default. Lets the
    // turrets be made tougher/weaker to the swarm at a glance, HP unchanged.
    def_set_turret_contact(g,
        getenv("VAT_HORDE_TURRET_DPS")?atof(getenv("VAT_HORDE_TURRET_DPS")):0.0f,
        getenv("VAT_HORDE_TURRET_REACH")?atof(getenv("VAT_HORDE_TURRET_REACH")):0.0f);
    if(designed){
        for(int k=0;k<sc->n_turret;k++){ const SceneTurret *st=&sc->turret[k];
            DefTurret t={0};
            t.x=st->x; t.y=st->y; t.ang=0.0f;
            t.arc_min=-3.1416f; t.arc_max=3.1416f;        // full sweep: engage anything in range
            t.sweep_dir=1; t.sweep_speed=3.0f; t.range=st->range;
            t.heavy=st->heavy; t.piercing=0;
            t.fire_period=st->heavy?0.5f:0.10f; t.damage=st->heavy?0.0f:55.0f;
            if(getenv("VAT_HORDE_TFIRE")) t.fire_period=atof(getenv("VAT_HORDE_TFIRE"));
            if(getenv("VAT_HORDE_TDMG"))  t.damage=atof(getenv("VAT_HORDE_TDMG"));
            int tid=def_add_turret(g,&t);
            float hp = st->hp>0.0f ? st->hp : turret_hp;   // per-turret HP, else default
            if(hp>0.0f) def_turret_make_destructible(g,tid,hp);
            placed++; }
        if(turret_hp>0.0f) simp_terrain_commit(s);        // commit turret walls
        printf("torrette (scena): %d%s\n", placed,
               turret_hp>0.0f?" (distruttibili)":"");
    } else {
        int nt_want=getenv("VAT_HORDE_TURRETS")?atoi(getenv("VAT_HORDE_TURRETS")):NT;
        if(nt_want>NT)nt_want=NT; if(nt_want<0)nt_want=0;
        for(int i=0;i<nt_want;i++){ float th=(float)i*(6.2831853f/(float)NT);
            float tx=bcx+TR_R*cosf(th), ty=bcy+TR_R*sinf(th);
            if(tx<1.0f||tx>sc->world_w-1.0f||ty<1.0f||ty>sc->world_h-1.0f) continue;
            DefTurret t={0};
            t.x=tx; t.y=ty; t.ang=th;
            float ha=3.15f; t.arc_min=th-ha; t.arc_max=th+ha;
            t.sweep_dir=1; t.sweep_speed=3.0f; t.range=55.0f;
            t.heavy=(i%4==2); t.piercing=(i%4==0);
            t.fire_period=t.heavy?0.5f:0.10f; t.damage=t.heavy?0.0f:55.0f;
            if(getenv("VAT_HORDE_THEAVY")) t.heavy=atoi(getenv("VAT_HORDE_THEAVY"));
            if(getenv("VAT_HORDE_TFIRE")) t.fire_period=atof(getenv("VAT_HORDE_TFIRE"));
            if(getenv("VAT_HORDE_TDMG"))  t.damage=atof(getenv("VAT_HORDE_TDMG"));
            def_add_turret(g,&t); placed++; }
        printf("torrette: %d piazzate (anello r=%.1f m attorno alla base (%.1f,%.1f))\n",
               placed,(double)TR_R,(double)bcx,(double)bcy);
    }

    // structures: scene `wall` entries (test maps) take priority; else the
    // legacy concentric base behind VAT_HORDE_BASE.
    if(sc->n_wall>0){ build_walls_from_scene(g,s,sc);
        printf("strutture (scena): %d muri distruttibili, bbox celle [%d,%d]-[%d,%d]\n",
               sc->n_wall, gStX0,gStY0,gStX1,gStY1); }
    else if(getenv("VAT_HORDE_BASE")){ build_base(g,s,sc->cell,bcx,bcy);
        printf("base: core HP %.0f + ring HP %.0f\n",
               (double)def_struct_hp_max(g,gCoreId),(double)def_struct_hp_max(g,gOuterId)); }

    if(fillN){ int got=prefill_lattice(s,g,vl,sc,fillN);
        printf("prefill: target %d -> %d agenti piazzati\n", fillN, got); }

    DefRect drects[16]; int ndr=sc->n_spawn<16?sc->n_spawn:16;
    for(int k=0;k<ndr;k++){ drects[k].x=sc->spawn[k].x; drects[k].y=sc->spawn[k].y;
        drects[k].w=sc->spawn[k].w; drects[k].h=sc->spawn[k].h; }
    DefDirector *dir=NULL;
    if(!fillN && ndr>0){ DefDirectorCfg dc={0};
        dc.rects=drects; dc.nrects=ndr; dc.spawn_radius=0.34f;
        dc.base_rate=getenv("VAT_HORDE_RATE")?atof(getenv("VAT_HORDE_RATE")):16.0f;
        dc.rate_ramp=8.0f; dc.wave_period=15.0f; dc.seed=0x5EED1234u;
        dc.on_spawn=on_director_spawn; dc.user=spctx;
        dir=def_director_create(g,&dc);
        printf("director: %d rect, base %.0f/s +%.0f/ondata (15s)\n",ndr,(double)dc.base_rate,(double)dc.rate_ramp); }

    *ps=s; *pg=g; *pdir=dir; return 0;
}

static void free_world(SimP *s, DefGame *g, DefDirector *dir){
    if(dir) def_director_destroy(dir);
    if(g)   def_destroy(g);
    if(s)   simp_destroy(s);
}

// Ricostruisce la mesh statica degli ostacoli nel VBO esistente (i poly della
// Scene cambiano in EDIT). Ritorna il nuovo conteggio vertici. Gli attrib del
// VAO restano validi (puntano allo stesso buffer).
static int upload_obstacle_mesh(GLuint vbo, const Scene *sc, int with_ground){
    int nv=0; float *m=build_obstacle_mesh(sc,with_ground,&nv);
    glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)nv*9*sizeof(float),m,GL_STATIC_DRAW);
    free(m); return nv;
}

int main(int argc, char **argv){
    const char *scene_path = argc > 1 ? argv[1] : "scenes/obstacles.scn";
    Scene sc;
    if (scene_load(scene_path, &sc) != 0) { fprintf(stderr, "scene load fail: %s\n", scene_path); return 1; }
    printf("scene %s: %dx%d cell=%g (%gx%g m) poly=%d spawn=%d goal=%d prop=%d\n",
           scene_path, sc.gw, sc.gh, (double)sc.cell, (double)sc.world_w, (double)sc.world_h,
           sc.n_poly, sc.n_spawn, sc.n_goal, sc.n_prop);

    // catalogo prop di decoro (§10 stadio 5b): tipo->mesh+scala+label. Render-only.
    const char *cpath = getenv("VAT_HORDE_PROPS") ? getenv("VAT_HORDE_PROPS") : "props/catalog.txt";
    gCatN = prop_catalog_load(cpath, &gCatalog);
    if (gCatN > 0) printf("prop catalog: %s (%d tipi)\n", cpath, gCatN);
    else { gCatN = 0; printf("prop catalog: %s assente -> tool prop disabilitato\n", cpath); }

    // terreno render-only (§9): glb dalla scena (o VAT_HORDE_TERRAIN), heightmap
    // .zhm a fianco (stesso path, estensione .zhm). Il .zhm è CPU-only → caricato
    // prima di SDL così i builder mesh statici lo campionano (ter_z).
    char terrain_glb[256]=""; const char *te=getenv("VAT_HORDE_TERRAIN");
    if(te) snprintf(terrain_glb,sizeof terrain_glb,"%s",te);
    else if(sc.terrain[0]) snprintf(terrain_glb,sizeof terrain_glb,"%s",sc.terrain);
    if(terrain_glb[0]){
        char zhm[256]; snprintf(zhm,sizeof zhm,"%s",terrain_glb);
        char *dot=strrchr(zhm,'.'); if(dot) strcpy(dot,".zhm"); else strncat(zhm,".zhm",sizeof zhm-strlen(zhm)-1);
        if(terrain_load(zhm,&gTer)==0){ gTerOn=1;
            printf("terreno: %s + %s (%dx%d @ %.1f px/m)\n",terrain_glb,zhm,gTer.w,gTer.h,(double)gTer.px_per_m); }
        else printf("terreno: %s ma .zhm mancante (%s) -> sprite su z=0\n",terrain_glb,zhm);
    }
    // glb#2: mesh visiva degli statici indistruttibili (palazzi/rocce, §9-§10).
    // Solo render (sempre, play+edit), zero effetto sim: il footprint nav è già
    // nei buchi del terreno (glb#1). Path da VAT_HORDE_STATICS o dalla scena.
    char statics_glb[256]=""; const char *se=getenv("VAT_HORDE_STATICS");
    if(se) snprintf(statics_glb,sizeof statics_glb,"%s",se);
    else if(sc.statics[0]) snprintf(statics_glb,sizeof statics_glb,"%s",sc.statics);
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
    if(vat_layer_nvariants(vl)!=NVAR){                 /* clamp silenzioso = body persi */
        fprintf(stderr,"VAT_MAX_VARIANTS (%d) < NVAR (%d): alza il limite in vat_layer.h\n",
                vat_layer_nvariants(vl),NVAR); return 1; }
    for(int v=0;v<NVAR;v++){ const VatMeta *M=vat_layer_meta_variant(vl,v);
        if(M->nclips<=0){fprintf(stderr,"meta vuoto: %s\n",PREFIX[v]);return 1;} }
    vat_layer_set_random_count(vl,NCOSMETIC);   /* crawler e tank solo via pin, non a caso */
    /* il tank ha raggio grosso per la massa fisica (collisione), ma e' alto
       ~2 m, non ~3 m come darebbe raggio/0.30: scala fissa 1.90-2.05 m
       (scala 1.0 = altezza-modello 1.8 m). */
    vat_layer_set_variant_scale(vl,TANK_VAR, 1.90f/1.8f, 2.05f/1.8f);
    printf("varianti=%d (di cui %d cosmetiche + arm + crawler + tank)\n",NVAR,NCOSMETIC);

    // Mondo vivo derivato dalla Scene (build_world): sim + torrette + base +
    // director. spctx persiste (il director ne tiene il puntatore come user).
    FxParticles fx; fx_init(&fx, 0xC0FFEEu);   // particle system (sangue), renderer-agnostic
    SpawnCtx spctx={NULL,vl,&fx};
    SimP *s=NULL; DefGame *g=NULL; DefDirector *dir=NULL;
    if(build_world(&sc, vl, fillN, &spctx, &s, &g, &dir)!=0) return 1;

    // prop distruttibili (DESTRUCT_DESIGN.md): l'orda li frantuma a contatto.
    Destruct dz; destruct_init(&dz, &sc, &gCatalog);
    DestructCtx dctx={&fx};

    // barricata di draggable per la verifica visiva (DRAG_DESIGN.md):
    // VAT_HORDE_BARRICADE="x,y,len[,mass]" piazza una fila accostata che l'orda sfonda.
    if(getenv("VAT_HORDE_BARRICADE")){ float bx=0,by=0,bl=8.0f,bm=14.0f;
        sscanf(getenv("VAT_HORDE_BARRICADE"),"%f,%f,%f,%f",&bx,&by,&bl,&bm);
        place_barricade(s,bx,by,bl,bm);
        printf("barricata: %d cassonetti @ (%.1f,%.1f) len %.1f massa %.1f\n",
               simp_drag_count(s),(double)bx,(double)by,(double)bl,(double)bm); }

    // auto = due dischi + rod rigido (DRAG_DESIGN.md §8) per la verifica visiva:
    // VAT_HORDE_CAR="x,y[,len][,mass]" piazza un'auto che l'orda spinge e fa ruotare.
    if(getenv("VAT_HORDE_CAR")){ float cx=0,cy=0,cl=3.0f,cm=20.0f;
        sscanf(getenv("VAT_HORDE_CAR"),"%f,%f,%f,%f",&cx,&cy,&cl,&cm);
        place_car(s,cx,cy,cl,cm);
        printf("auto: rod len %.1f massa %.1f @ (%.1f,%.1f), %d giunti\n",
               (double)cl,(double)cm,(double)cx,(double)cy,simp_drag_link_count(s)); }

    const char *shot=getenv("VAT_HORDE_SHOT");
    int shot_frames = shot? atoi(shot):0; if(shot&&shot_frames<=0)shot_frames=600;

    // blast headless: VAT_HORDE_BLAST="frame,x,y[,strength,up]" → esplosione con
    // lancio (apply_impulse_ex) a quel frame; per verificare il volo balistico.
    int blast_frame=-1; float blast_x=0,blast_y=0,blast_str=28.0f,blast_up=0.7f;
    if(getenv("VAT_HORDE_BLAST")){ sscanf(getenv("VAT_HORDE_BLAST"),"%d,%f,%f,%f,%f",
        &blast_frame,&blast_x,&blast_y,&blast_str,&blast_up); }
    int blast_pending=0;   // armato da E (interattivo) o dall'env
    int SW=1280,SH=720;

    if(!SDL_Init(SDL_INIT_VIDEO)){fprintf(stderr,"SDL:%s\n",SDL_GetError());return 1;}
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,24);
    SDL_Window*win=SDL_CreateWindow("vat_horde",SW,SH,SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE);
    int fullscreen=0;
    SDL_GLContext ctx=SDL_GL_CreateContext(win);
    if(!ctx||!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)){fprintf(stderr,"GL init fail\n");return 1;}
    printf("GL %s\n",glGetString(GL_VERSION));

    GLuint prog=vg_shader("vat/vat.vs","vat/vat.fs");
    // instance buffer condiviso: ri-riempito per variante prima di ogni draw.
    GLuint bi; glGenBuffers(1,&bi);glBindBuffer(GL_ARRAY_BUFFER,bi);
    glBufferData(GL_ARRAY_BUFFER,sizeof(inst),NULL,GL_DYNAMIC_DRAW);

    // --- terreno glb (render-only): carica la mesh del suolo + shader texturizzato.
    Ground gnd; int groundOn=0;
    GLuint progGnd=0; GLint uVPgnd=0,uHasGnd=0,uColGnd=0;
    if(terrain_glb[0] && load_ground_glb(terrain_glb,&gnd)==0){ groundOn=1;
        progGnd=vg_shader("vat/ground.vs","vat/ground.fs");
        uVPgnd=glGetUniformLocation(progGnd,"uVP");
        uHasGnd=glGetUniformLocation(progGnd,"uHasTex");
        uColGnd=glGetUniformLocation(progGnd,"uColor");
        glUniform1i(glGetUniformLocation(progGnd,"uTex"),0); }
    // glb#2 statici: stessa mesh/shader del terreno, disegnato sempre. Se manca
    // il terreno (no progGnd) compiliamo comunque lo shader per disegnarlo.
    Ground gStat; int staticsOn=0;
    if(statics_glb[0] && load_ground_glb(statics_glb,&gStat)==0){ staticsOn=1;
        if(!progGnd){ progGnd=vg_shader("vat/ground.vs","vat/ground.fs");
            uVPgnd=glGetUniformLocation(progGnd,"uVP");
            uHasGnd=glGetUniformLocation(progGnd,"uHasTex");
            uColGnd=glGetUniformLocation(progGnd,"uColor");
            glUniform1i(glGetUniformLocation(progGnd,"uTex"),0); }
        printf("statici: %s (%d idx)%s\n",statics_glb,gStat.nidx,gStat.hasTex?" texturizzato":""); }

    // --- ombre a terra: un disco unitario instanziato sotto ogni agente alla
    // quota del terreno (blob morbido, blend). Ground reale (terrain_z) anche
    // sotto chi vola → l'ombra resta a terra.
#define SHADN 18
    float disc[SHADN*2]; disc[0]=0; disc[1]=0;        // fan: centro + 16 + chiusura
    for(int i=0;i<SHADN-1;i++){ float a=(float)i*(6.2831853f/16.0f);
        disc[(i+1)*2]=cosf(a); disc[(i+1)*2+1]=sinf(a); }
    GLuint progSh=vg_shader("vat/shadow.vs","vat/shadow.fs");
    GLint uVPsh=glGetUniformLocation(progSh,"uVP");
    GLuint shVao,shDisc,shInst; glGenVertexArrays(1,&shVao);glBindVertexArray(shVao);
    glGenBuffers(1,&shDisc);glBindBuffer(GL_ARRAY_BUFFER,shDisc);
    glBufferData(GL_ARRAY_BUFFER,sizeof disc,disc,GL_STATIC_DRAW);
    glVertexAttribPointer(0,2,GL_FLOAT,0,2*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glGenBuffers(1,&shInst);glBindBuffer(GL_ARRAY_BUFFER,shInst);
    glBufferData(GL_ARRAY_BUFFER,sizeof shad,NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(1,4,GL_FLOAT,0,4*sizeof(float),(void*)0);glEnableVertexAttribArray(1);glVertexAttribDivisor(1,1);
    glBindVertexArray(0);

    // --- decal di sangue (GFX §5.2): disco unitario instanziato (riusa la geom
    // del disco ombra shDisc), shader proprio (rosso, colore per-istanza),
    // persistenti. Pool renderer-side in vat_layer.
#define DECALMAX 8192
    static float decalraw[DECALMAX*6];           // x,y,size,r,g,b (da vat_layer)
    static float decalinst[DECALMAX*7];          // cx,gy,cz,radius,r,g,b (istanza GL)
    GLuint progDc=vg_shader("vat/decal.vs","vat/decal.fs");
    GLint uVPdc=glGetUniformLocation(progDc,"uVP");
    GLuint dcVao,dcInst; glGenVertexArrays(1,&dcVao);glBindVertexArray(dcVao);
    glBindBuffer(GL_ARRAY_BUFFER,shDisc);        // condivide la geom del disco
    glVertexAttribPointer(0,2,GL_FLOAT,0,2*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glGenBuffers(1,&dcInst);glBindBuffer(GL_ARRAY_BUFFER,dcInst);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)DECALMAX*7*sizeof(float),NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(1,4,GL_FLOAT,0,7*sizeof(float),(void*)0);glEnableVertexAttribArray(1);glVertexAttribDivisor(1,1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,7*sizeof(float),(void*)(4*sizeof(float)));glEnableVertexAttribArray(2);glVertexAttribDivisor(2,1);
    glBindVertexArray(0);

    // --- sagome-cadavere (richiesta utente / CORPSE_DESIGN §10.2): quad orientati
    // texturizzati con l'atlante di sprite top-down bakato dai modelli VAT in posa
    // di morte (bake init-time più sotto). Persistenti come i decal di sangue.
#define CORPSEDECMAX 4096
#define CORPSE_CELL  256                  // px per cella d'atlante
#define CORPSE_HALF  1.15f                // semi-lato ortho del bake (m): un corpo disteso
    static float cdecraw[CORPSEDECMAX*6];  // x,y,hd,size,colonna,outfit (da vat_layer)
    static float cdecinst[CORPSEDECMAX*7]; // cx,gy,cz,hd, half,colonna,outfit (istanza GL)
    static const float cquad[12]={-1,-1, 1,-1, 1,1, -1,-1, 1,1, -1,1};
    GLuint cdProg=vg_shader("vat/corpse_decal.vs","vat/corpse_decal.fs");
    GLint uVPcd=glGetUniformLocation(cdProg,"uVP"), uNColsCd=glGetUniformLocation(cdProg,"uNCols"),
          uNOutCd=glGetUniformLocation(cdProg,"uNOutfit"), uNormLitCd=glGetUniformLocation(cdProg,"uNormalLit");
    GLuint cdVao,cdQuad,cdInst; glGenVertexArrays(1,&cdVao);glBindVertexArray(cdVao);
    glGenBuffers(1,&cdQuad);glBindBuffer(GL_ARRAY_BUFFER,cdQuad);
    glBufferData(GL_ARRAY_BUFFER,sizeof cquad,cquad,GL_STATIC_DRAW);
    glVertexAttribPointer(0,2,GL_FLOAT,0,2*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glGenBuffers(1,&cdInst);glBindBuffer(GL_ARRAY_BUFFER,cdInst);
    glBufferData(GL_ARRAY_BUFFER,sizeof cdecinst,NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(1,4,GL_FLOAT,0,7*sizeof(float),(void*)0);glEnableVertexAttribArray(1);glVertexAttribDivisor(1,1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,7*sizeof(float),(void*)(4*sizeof(float)));glEnableVertexAttribArray(2);glVertexAttribDivisor(2,1);
    glBindVertexArray(0);
    GLuint corpseAlb=0,corpseNrm=0;            // albedo (griglia colonna×outfit) + normal (riga)
    int corpseNCols=NVAR*VAT_CORPSE_NPOSE, corpseNOut=VAT_CORPSE_NOUTFIT;

    // --- ostacoli: programma flat + mesh statica estrusa dalla scena (il quad
    // suolo flat si salta quando c'è il terreno glb).
    GLuint progFlat=vg_shader("vat/flat.vs","vat/flat.fs");
    GLint uVPflat=glGetUniformLocation(progFlat,"uVP");
    int obNV=0; float *obMesh=build_obstacle_mesh(&sc,!groundOn,&obNV);
    GLuint obVao,obVbo; glGenVertexArrays(1,&obVao);glBindVertexArray(obVao);
    glGenBuffers(1,&obVbo);glBindBuffer(GL_ARRAY_BUFFER,obVbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)obNV*9*sizeof(float),obMesh,GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,9*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,0,9*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,9*sizeof(float),(void*)(6*sizeof(float)));glEnableVertexAttribArray(2);
    glBindVertexArray(0); free(obMesh);
    printf("ostacoli: %d triangoli\n",obNV/3);

    // prop di decoro (§10 stadio 5b): mesh placeholder bakata dalla Scene, flat
    // shader. Render-only (play+edit), ricostruita su ogni edit dei prop.
    int prNV=0; float *prMesh=build_prop_mesh(&sc,&gCatalog,&dz,&prNV);
    GLuint prVao,prVbo; glGenVertexArrays(1,&prVao);glBindVertexArray(prVao);
    glGenBuffers(1,&prVbo);glBindBuffer(GL_ARRAY_BUFFER,prVbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(prNV>0?prNV:1)*9*sizeof(float),prMesh,GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,9*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,0,9*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,9*sizeof(float),(void*)(6*sizeof(float)));glEnableVertexAttribArray(2);
    glBindVertexArray(0); free(prMesh);
    printf("prop: %d istanze (%d triangoli)\n",sc.n_prop,prNV/3);

    // overlay editor (rect/poly/cursore): stesso layout del flat shader, dinamico.
    GLuint ovVao,ovVbo; glGenVertexArrays(1,&ovVao);glBindVertexArray(ovVao);
    glGenBuffers(1,&ovVbo);glBindBuffer(GL_ARRAY_BUFFER,ovVbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof edovl,NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,9*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,0,9*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,9*sizeof(float),(void*)(6*sizeof(float)));glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    // --- gib (GFX §5 gore): mesh dinamica di pezzi-box, ricostruita CPU ogni
    // frame (come tracer/overlay), flat shader. Pool renderer-side in vat_layer.
#define GIBMAX 2048
#define GIB_VERTS_EACH 30            // prop_box: top(6) + 4 pareti(24)
    static float gibparam[GIBMAX*10];               // x,y,z,hx,hy,hz,ang,r,g,b
    float *gibmesh = malloc((size_t)GIBMAX*GIB_VERTS_EACH*9*sizeof(float));
    GLuint gibVao,gibVbo; glGenVertexArrays(1,&gibVao);glBindVertexArray(gibVao);
    glGenBuffers(1,&gibVbo);glBindBuffer(GL_ARRAY_BUFFER,gibVbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)GIBMAX*GIB_VERTS_EACH*9*sizeof(float),NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,9*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,0,9*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,9*sizeof(float),(void*)(6*sizeof(float)));glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    // stato editor (EDITOR_DESIGN fase 1): VAT_HORDE_EDIT=1 parte in EDIT, TAB
    // commuta. In EDIT la sim NON steppa, si edita la Scene e si salva (F2).
    Editor ed; ed_init(&ed); ed.active = getenv("VAT_HORDE_EDIT")?1:0;
    if(gCatN>0){ snprintf(ed.prop_key,sizeof ed.prop_key,"%s",gCatalog.defs[0].key); }

    // torrette (DINAMICO: una distruttibile sparisce al collasso) + tracer di
    // fuoco, stesso flat shader. Buffer riusato ogni frame da build_turret_mesh.
    int turCap=def_turret_count(g); if(turCap<NT) turCap=NT;   // >= tracer's NT cap
    float *turBuf=malloc((size_t)turCap*30*9*sizeof(float));
    GLuint turVao,turVbo; glGenVertexArrays(1,&turVao);glBindVertexArray(turVao);
    glGenBuffers(1,&turVbo);glBindBuffer(GL_ARRAY_BUFFER,turVbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)turCap*30*9*sizeof(float),NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,9*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,0,9*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,9*sizeof(float),(void*)(6*sizeof(float)));glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    // draggable (DRAG_DESIGN.md): box per oggetto, rebuilt ogni frame, flat shader
    float *dragBuf=malloc((size_t)DRAG_DRAW_CAP*DRAG_VERTS_EACH*9*sizeof(float));
    GLuint dragVao,dragVbo; glGenVertexArrays(1,&dragVao);glBindVertexArray(dragVao);
    glGenBuffers(1,&dragVbo);glBindBuffer(GL_ARRAY_BUFFER,dragVbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)DRAG_DRAW_CAP*DRAG_VERTS_EACH*9*sizeof(float),NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,9*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,0,9*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,9*sizeof(float),(void*)(6*sizeof(float)));glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    GLuint trVao,trVbo; glGenVertexArrays(1,&trVao);glBindVertexArray(trVao);
    glGenBuffers(1,&trVbo);glBindBuffer(GL_ARRAY_BUFFER,trVbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)NT*2*9*sizeof(float),NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,9*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,0,9*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,9*sizeof(float),(void*)(6*sizeof(float)));glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    // strutture della base (dinamico: ricostruito ogni frame dallo stato vivo).
    int stMaxV = gStructOn ? (gStX1-gStX0+3)*(gStY1-gStY0+3)*30 : 0;
    float *stBuf = stMaxV ? malloc((size_t)stMaxV*9*sizeof(float)) : NULL;
    GLuint stVao=0,stVbo=0;
    if(gStructOn){ glGenVertexArrays(1,&stVao);glBindVertexArray(stVao);
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
        snprintf(mesh,256,"%s_mesh.bin",PREFIX[v]);
        snprintf(diff,256,"%s_diffuse.png", PREFIX[v]);

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

    // --- BAKE atlante sagome-cadavere (init-time): ortho TOP-DOWN del modello VAT
    // nell'ultimo frame della posa di morte. ALBEDO = griglia (colonna=var*posa ×
    // riga=outfit), NORMAL = una sola riga (outfit-indipendente) per il relight
    // per-pixel. Colonna = v*NPOSE+p; posa 0='dying', 1='death' (devono combaciare
    // con vat_layer_die, le due falle a terra differiscono di ~90°).
    GLuint bakeProg=vg_shader("vat/vat.vs","vat/corpsebake.fs");
    GLint uVPb=glGetUniformLocation(bakeProg,"uVP"),uTSb=glGetUniformLocation(bakeProg,"texSize"),
          uRPFb=glGetUniformLocation(bakeProg,"rowsPerFrame"),uHasb=glGetUniformLocation(bakeProg,"uHasTex"),
          uModeb=glGetUniformLocation(bakeProg,"uMode");
    {   int Wc=CORPSE_CELL*corpseNCols, Hg=CORPSE_CELL*corpseNOut;     // albedo grid
        GLuint *tex[2]={&corpseAlb,&corpseNrm}; int texH[2]={Hg,CORPSE_CELL};
        for(int t=0;t<2;t++){
            glGenTextures(1,tex[t]);glBindTexture(GL_TEXTURE_2D,*tex[t]);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,Wc,texH[t],0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        }
        mat4 cproj,cview,cvp; m_ortho(cproj,-CORPSE_HALF,CORPSE_HALF,-CORPSE_HALF,CORPSE_HALF,-10,10);
        float ceye[3]={0,5,0},cctr[3]={0,0,0},cup[3]={0,0,1};
        m_lookat(cview,ceye,cctr,cup); m_mul(cvp,cproj,cview);
        glUseProgram(bakeProg); glUniformMatrix4fv(uVPb,1,GL_FALSE,cvp);
        glUniform1i(glGetUniformLocation(bakeProg,"texPos"),0);
        glUniform1i(glGetUniformLocation(bakeProg,"texNorm"),1);
        glUniform1i(glGetUniformLocation(bakeProg,"texDiff"),2);
        glDisable(GL_BLEND);
        const char *POSE_PFX[VAT_CORPSE_NPOSE]={"dying","death"};
        // due passate: 0 = ALBEDO (per outfit, griglia corpseAlb), 1 = NORMAL (riga).
        for(int pass=0;pass<2;pass++){
            GLuint dst=pass?corpseNrm:corpseAlb; int Hh=pass?CORPSE_CELL:Hg;
            int nout=pass?1:corpseNOut;
            GLuint fbo,depth; glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,dst,0);
            glGenRenderbuffers(1,&depth);glBindRenderbuffer(GL_RENDERBUFFER,depth);
            glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,Wc,Hh);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth);
            if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) fprintf(stderr,"corpse atlas FBO incompleto\n");
            glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
            glUniform1i(uModeb,pass);
            for(int v=0;v<NVAR;v++){
                const VatMeta *M=A[v].M;
                glUniform2f(uTSb,(float)M->texW,(float)M->texH);glUniform1f(uRPFb,(float)M->rowsPerFrame);
                glUniform1i(uHasb,A[v].hasTex);
                glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,A[v].texP);
                glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,A[v].texN);
                glActiveTexture(GL_TEXTURE2);glBindTexture(GL_TEXTURE_2D,A[v].texD);
                glBindVertexArray(A[v].vao);
                for(int p=0;p<VAT_CORPSE_NPOSE;p++){
                    int fr=-1;
                    for(int ci=0;ci<M->nclips;ci++) if(!strncmp(M->clip[ci].name,POSE_PFX[p],5)){
                        fr=M->clip[ci].startFrame+M->clip[ci].numFrames-1; break; }
                    if(fr<0) fr=0;                        // posa mancante (raro/crawler): cella inerte
                    int col=v*VAT_CORPSE_NPOSE+p;
                    for(int o=0;o<nout;o++){
                        // riga o = outfit base; bake con la versione INSANGUINATA (o+16)
                        float one[12]={0,0,0, 0, 1.0f, (float)fr,(float)fr,0, (float)(o+16), 0.55f,0.5f,0.5f};
                        glViewport(col*CORPSE_CELL,o*CORPSE_CELL,CORPSE_CELL,CORPSE_CELL);
                        glBindBuffer(GL_ARRAY_BUFFER,bi);glBufferSubData(GL_ARRAY_BUFFER,0,12*sizeof(float),one);
                        glDrawElementsInstanced(GL_TRIANGLES,A[v].ni,GL_UNSIGNED_SHORT,0,1);
                    }
                }
            }
            // debug: VAT_HORDE_CORPSE_ATLAS -> dump RGB (sagoma su magenta) della passata
            if(getenv("VAT_HORDE_CORPSE_ATLAS")){
                unsigned char *rgba=malloc((size_t)Wc*Hh*4); glReadPixels(0,0,Wc,Hh,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
                unsigned char *rgb=malloc((size_t)Wc*Hh*3);
                for(int i=0;i<Wc*Hh;i++){ if(rgba[i*4+3]>10){rgb[i*3]=rgba[i*4];rgb[i*3+1]=rgba[i*4+1];rgb[i*3+2]=rgba[i*4+2];}
                    else {rgb[i*3]=255;rgb[i*3+1]=0;rgb[i*3+2]=255;} }
                char nm[64]; snprintf(nm,64,"corpse_atlas_%s.bmp",pass?"normal":"albedo");
                vg_save_bmp(nm,Wc,Hh,rgb); free(rgba);free(rgb);
                printf("corpse atlas -> %s (%dx%d)\n",nm,Wc,Hh);
            }
            glBindFramebuffer(GL_FRAMEBUFFER,0);
            glDeleteRenderbuffers(1,&depth);glDeleteFramebuffers(1,&fbo);
        }
    }

    float cx=sc.world_w*0.5f, cz=sc.world_h*0.30f, hh=15.0f, az=0.7f, el=0.40f; int cam_free=0,paused=0,useTex=1;
    // camera col mouse: PLAY = LMB pan / RMB rotate / wheel zoom; EDIT = Alt+LMB
    // pan / Alt+RMB rotate (LMB/RMB nudi = strumenti). Pan via anchor (il punto a
    // terra sotto il cursore resta incollato), rotate via delta pixel, una volta
    // per frame con la VP del frame precedente.
    int   drag_cam=0;                 // 0 nessuno, 1 pan, 2 rotate
    float drag_anx=0,drag_any=0;      // ancora mondo del pan
    float mouse_px=0,mouse_py=0;      // ultimo cursore in pixel
    float rot_px=0,rot_py=0;          // pixel di riferimento del rotate
    { const char*cs=getenv("VAT_HORDE_CAM"); if(cs) sscanf(cs,"%f,%f,%f,%f,%f",&cx,&cz,&hh,&az,&el); }

    // --- gore mesh-gibs (FX_LAB): VAO delle mesh di blend/gibs.glb + shader mesh
    // statica texturizzata (diffuse di zombie_man, A[0]). Pool fisico in vat_layer.
    GibMesh GM[8]={0}; int nGibMesh=load_gib_meshes("blend/gibs.glb",GM,8);
    GLuint mProg=vg_shader("vat/mesh.vs","vat/mesh.fs");
    GLint uVPm=glGetUniformLocation(mProg,"uVP"), uModelm=glGetUniformLocation(mProg,"uModel");
    static float meshgib[256*9];
    printf("mesh-gib: %d mesh da blend/gibs.glb\n",nGibMesh);

    // --- particle system (sangue): billboard istanziati, due passate (alpha/add).
    static float pmat[FX_MAX_PARTICLES*16], pcol[FX_MAX_PARTICLES*4], pspr[FX_MAX_PARTICLES];
    static const float pquad[18]={-0.5f,-0.5f,0, 0.5f,-0.5f,0, 0.5f,0.5f,0,
                                  -0.5f,-0.5f,0, 0.5f,0.5f,0, -0.5f,0.5f,0};
    GLuint pProg=vg_shader("vat/particle.vs","vat/particle.fs");
    GLint uVPp=glGetUniformLocation(pProg,"uVP"), uHasAtl=glGetUniformLocation(pProg,"uHasAtlas"),
          uAtlGrid=glGetUniformLocation(pProg,"uAtlasGrid");
    GLuint pVao,pQuadVbo,pMatVbo,pColVbo,pSprVbo;
    glGenVertexArrays(1,&pVao); glBindVertexArray(pVao);
    glGenBuffers(1,&pQuadVbo); glBindBuffer(GL_ARRAY_BUFFER,pQuadVbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof pquad,pquad,GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,3*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glGenBuffers(1,&pMatVbo); glBindBuffer(GL_ARRAY_BUFFER,pMatVbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof pmat,NULL,GL_DYNAMIC_DRAW);
    for(int cc=0;cc<4;cc++){ glVertexAttribPointer(1+cc,4,GL_FLOAT,0,16*sizeof(float),(void*)(cc*4*sizeof(float)));
        glEnableVertexAttribArray(1+cc); glVertexAttribDivisor(1+cc,1); }
    glGenBuffers(1,&pColVbo); glBindBuffer(GL_ARRAY_BUFFER,pColVbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof pcol,NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(5,4,GL_FLOAT,0,4*sizeof(float),(void*)0);glEnableVertexAttribArray(5);glVertexAttribDivisor(5,1);
    glGenBuffers(1,&pSprVbo); glBindBuffer(GL_ARRAY_BUFFER,pSprVbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof pspr,NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(6,1,GL_FLOAT,0,sizeof(float),(void*)0);glEnableVertexAttribArray(6);glVertexAttribDivisor(6,1);
    glBindVertexArray(0);

    Uint64 pf=SDL_GetPerformanceFrequency();
    // timestep FISSO disaccoppiato dal framerate: accumulo il tempo reale e lo
    // consumo in passi da 1/60 → la sim avanza alla stessa velocità a 60 o 144 Hz
    // (il vsync governa i frame, non i passi di simulazione).
    const float FIXED_DT=1.0f/60.0f;
    mat4 vp; m_identity(vp);     // hoisted: il picking nel mouse handler usa la VP del frame precedente
    Uint64 prev=SDL_GetPerformanceCounter(); double acc_t=0.0;
    double acc_sim=0,acc_lay=0,acc_ren=0; int acc_n=0;
    double bsim=0,blay=0,bren=0; int bn=0;     // finestra di misura benchmark
    int running=1,frame=0,shot_done=0;
    while(running){
        SDL_Event e; while(SDL_PollEvent(&e)){
            if(e.type==SDL_EVENT_QUIT){ running=0; continue; }
            // --- rotellina = zoom (entrambe le modalità) ---
            if(e.type==SDL_EVENT_MOUSE_WHEEL){
                hh *= (e.wheel.y>0)?0.9f:1.1f;
                if(hh<2.0f)hh=2.0f; if(hh>400.0f)hh=400.0f; continue; }
            // --- mouse (motion/button) ---
            if(e.type==SDL_EVENT_MOUSE_MOTION ||
               e.type==SDL_EVENT_MOUSE_BUTTON_DOWN ||
               e.type==SDL_EVENT_MOUSE_BUTTON_UP){
                // punto logico -> pixel (corretto anche su HiDPI: SW/SH sono pixel)
                int wpw=1,wph=1; SDL_GetWindowSize(win,&wpw,&wph);
                int motion=(e.type==SDL_EVENT_MOUSE_MOTION);
                float mxf=(motion?e.motion.x:e.button.x)*(float)SW/(wpw>0?wpw:1);
                float myf=(motion?e.motion.y:e.button.y)*(float)SH/(wph>0?wph:1);
                if(motion){ mouse_px=mxf; mouse_py=myf; }
                int alt = (SDL_GetModState()&SDL_KMOD_ALT)!=0;
                // camera col mouse? PLAY: LMB/RMB nudi; EDIT: solo con Alt.
                int cam_gesture = (!ed.active) || alt;
                if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN && cam_gesture){
                    if(e.button.button==SDL_BUTTON_LEFT){      // pan: ancora il punto sotto il cursore
                        drag_cam=1; float ax,ay;
                        if(pick_y0(vp,mxf,myf,SW,SH,&ax,&ay)){ drag_anx=ax; drag_any=ay; }
                    } else if(e.button.button==SDL_BUTTON_RIGHT){ drag_cam=2; rot_px=mxf; rot_py=myf; }
                    continue;
                }
                if(e.type==SDL_EVENT_MOUSE_BUTTON_UP && drag_cam){ drag_cam=0; continue; }
                if(motion && drag_cam) continue;               // pan/rotate applicati nel frame body
                // --- da qui in giù: strumenti editor (solo in EDIT, mouse nudo) ---
                if(!ed.active) continue;
                float wx=0,wy=0; int hit=pick_y0(vp,mxf,myf,SW,SH,&wx,&wy);
                if(hit){ ed.curx=wx; ed.cury=wy; ed.have_cursor=1; }
                if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN && hit){
                    // veto: niente piazzamento sopra uno statico (palazzo/roccia)
                    if(e.button.button==SDL_BUTTON_LEFT && !ter_blocked(wx,wy)){
                        if(ed.tool==ED_GOAL||ed.tool==ED_SPAWN||ed.tool==ED_COST||ed.tool==ED_PACK){
                            ed.dragging=1; ed.ax=ed.bx=wx; ed.ay=ed.by=wy;
                        } else if(ed.tool==ED_WALL||ed.tool==ED_COSTPOLY){
                            ed_poly_vertex(&ed,&sc,wx,wy);
                        } else if(ed.tool==ED_PROP){
                            if(ed_place_prop(&ed,&sc,wx,wy)){ destruct_init(&dz,&sc,&gCatalog); prNV=upload_prop_mesh(prVbo,&sc,&gCatalog,&dz); }
                        }
                    } else if(e.button.button==SDL_BUTTON_RIGHT){
                        if(ed.npoly>0) ed.npoly--;                 // annulla ultimo vertice
                        else if(ed_delete_at(&sc,wx,wy)){ ed.dirty=1;
                            obNV=upload_obstacle_mesh(obVbo,&sc,!groundOn);
                            destruct_init(&dz,&sc,&gCatalog);
                            prNV=upload_prop_mesh(prVbo,&sc,&gCatalog,&dz); }
                    }
                } else if(motion && ed.dragging && hit){ ed.bx=wx; ed.by=wy; }
                else if(e.type==SDL_EVENT_MOUSE_BUTTON_UP &&
                        e.button.button==SDL_BUTTON_LEFT && ed.dragging){
                    ed_commit_drag(&ed,&sc);
                }
                continue;
            }
            if(e.type!=SDL_EVENT_KEY_DOWN) continue;
            // --- tasti globali (EDIT e PLAY) ---
            switch(e.key.key){
                case SDLK_ESCAPE: running=0; break;
                case SDLK_F11: fullscreen=!fullscreen; SDL_SetWindowFullscreen(win,fullscreen); break;
                case SDLK_TAB:                                    // EDIT<->PLAY
                    if(ed.active){
                        if(ed.dirty){ free_world(s,g,dir);        // re-instanzia dalla Scene editata
                            if(build_world(&sc,vl,fillN,&spctx,&s,&g,&dir)!=0){running=0;break;}
                            destruct_init(&dz,&sc,&gCatalog);     // ri-stato distruttibili
                            obNV=upload_obstacle_mesh(obVbo,&sc,!groundOn);
                            prNV=upload_prop_mesh(prVbo,&sc,&gCatalog,&dz); ed.dirty=0; }
                        ed.active=0;
                    } else ed.active=1;
                    break;
                default: break;
            }
            if(ed.active) switch(e.key.key){                      // --- tasti EDIT ---
                case SDLK_1: ed.tool=ED_SELECT; break;
                case SDLK_2: ed.tool=ED_GOAL;   break;
                case SDLK_3: ed.tool=ED_SPAWN;  break;
                case SDLK_4: ed.tool=ED_COST;   break;
                case SDLK_5: ed.tool=ED_PACK;   break;
                case SDLK_6: ed.tool=ED_WALL;   break;
                case SDLK_7: ed.tool=ED_COSTPOLY; break;
                case SDLK_8: if(gCatN>0) ed.tool=ED_PROP; break;
                case SDLK_G: ed.snap=!ed.snap;  break;
                case SDLK_RETURN:
                    if((ed.tool==ED_WALL||ed.tool==ED_COSTPOLY) && ed_poly_close(&ed,&sc))
                        obNV=upload_obstacle_mesh(obVbo,&sc,!groundOn);
                    break;
                case SDLK_BACKSPACE: if(ed.npoly>0) ed.npoly--; break;
                case SDLK_LEFTBRACKET:
                    if(ed.tool==ED_PROP){ if(gCatN>0){ ed.prop_idx=(ed.prop_idx+gCatN-1)%gCatN;
                        snprintf(ed.prop_key,sizeof ed.prop_key,"%s",gCatalog.defs[ed.prop_idx].key); } }
                    else if(ed.tool==ED_WALL) ed.poly_h=fmaxf(0.2f,ed.poly_h-0.5f);
                    else if(ed.tool==ED_COSTPOLY) ed.poly_cost=fmaxf(0.0f,ed.poly_cost-1.0f);
                    else ed.rect_cost=fmaxf(0.0f,ed.rect_cost-1.0f);
                    break;
                case SDLK_RIGHTBRACKET:
                    if(ed.tool==ED_PROP){ if(gCatN>0){ ed.prop_idx=(ed.prop_idx+1)%gCatN;
                        snprintf(ed.prop_key,sizeof ed.prop_key,"%s",gCatalog.defs[ed.prop_idx].key); } }
                    else if(ed.tool==ED_WALL) ed.poly_h+=0.5f;
                    else if(ed.tool==ED_COSTPOLY) ed.poly_cost+=1.0f;
                    else ed.rect_cost+=1.0f;
                    break;
                case SDLK_COMMA:  if(ed.tool==ED_PROP) ed.prop_rot=fmodf(ed.prop_rot-15.0f+360.0f,360.0f); break;
                case SDLK_PERIOD: if(ed.tool==ED_PROP) ed.prop_rot=fmodf(ed.prop_rot+15.0f,360.0f); break;
                case SDLK_F2:
                    if(scene_save(scene_path,&sc)==0) printf("salvato %s\n",scene_path);
                    else fprintf(stderr,"save fail %s\n",scene_path);
                    break;
                default: break;
            } else switch(e.key.key){                             // --- tasti PLAY ---
                case SDLK_C:cam_free=!cam_free;break; case SDLK_T:useTex=!useTex;break;
                case SDLK_SPACE:paused=!paused;break;
                case SDLK_E:blast_x=cx;blast_y=cz;blast_pending=1;break;
                case SDLK_B:{   // piazza un cassonetto draggable sotto il cursore
                    float wx,wy;
                    if(pick_y0(vp,mouse_px,mouse_py,SW,SH,&wx,&wy)){
                        float m=(e.key.mod&SDL_KMOD_SHIFT)?30.0f:12.0f;
                        if(simp_drag_add(s,wx,wy,0.6f,m)>=0)
                            printf("cassonetto @ (%.1f,%.1f) massa %.0f (tot %d)\n",
                                   (double)wx,(double)wy,(double)m,simp_drag_count(s)); }
                    break; }
                case SDLK_N:{   // piazza un'AUTO (2 dischi + rod, DRAG_DESIGN.md §8)
                    float wx,wy;
                    if(pick_y0(vp,mouse_px,mouse_py,SW,SH,&wx,&wy)){
                        float m=(e.key.mod&SDL_KMOD_SHIFT)?40.0f:20.0f;
                        place_car(s,wx,wy,3.0f,m);
                        printf("auto @ (%.1f,%.1f) massa %.0f (giunti %d)\n",
                               (double)wx,(double)wy,(double)m,simp_drag_link_count(s)); }
                    break; }
                default: break;
            }
            switch(e.key.key){                                    // --- camera (sempre) ---
                case SDLK_LEFT:az-=0.06f;break; case SDLK_RIGHT:az+=0.06f;break;
                case SDLK_UP:el+=0.04f;break; case SDLK_DOWN:el-=0.04f;break;
                case SDLK_EQUALS:case SDLK_PLUS:hh*=0.9f;break; case SDLK_MINUS:hh*=1.1f;break;
                default: break;
            }
        }
        double sim_ms=0, lay_ms=0;
        // tempo del frame: reale (interattivo) o fisso 1/60 (headless shot/bench →
        // 1 passo/frame, deterministico e indipendente dal tempo a parete).
        Uint64 nowc=SDL_GetPerformanceCounter();
        double frame_t=(double)(nowc-prev)/(double)pf; prev=nowc;
        if(shot || bench_meas) frame_t=FIXED_DT;
        if(frame_t>0.25) frame_t=0.25;                 // anti spiral-of-death
        if(blast_frame>=0 && frame>=blast_frame){ blast_pending=1; blast_frame=-1; }
        if(!ed.active && !paused){
            if(blast_pending){ simp_apply_impulse_ex(s,blast_x,blast_y,8.0f,blast_str,blast_up);
                blast_pending=0; printf("blast @ (%.1f,%.1f) str %.1f up %.2f\n",
                    (double)blast_x,(double)blast_y,(double)blast_str,(double)blast_up); }
            acc_t+=frame_t;
            while(acc_t>=FIXED_DT){
                // §8: il director emette l'ondata (burst-free). In benchmark
                // (fillN) il campo è già pieno: niente director.
                if(dir && simp_count(s)<MAXA) def_director_update(dir,FIXED_DT);
                Uint64 t0=SDL_GetPerformanceCounter();
                simp_step(s,FIXED_DT);
                Uint64 t1=SDL_GetPerformanceCounter();
                def_update(g,FIXED_DT);
                // prop distruttibili (DESTRUCT_DESIGN.md): contatto -> scoppio FX.
                // dopo def_update (la griglia puo' essere stantia dai kill: la query
                // cade su brute-force, corretta). Re-upload se cambia o sta cadendo.
                if(destruct_update(&dz,s,&sc,&gCatalog,FIXED_DT,on_prop_burst,&dctx)
                   || destruct_animating(&dz))
                    prNV=upload_prop_mesh(prVbo,&sc,&gCatalog,&dz);
                // i feriti si insanguinano (outfit+16); i maimed_legs passano
                // anche alla mesh crawler (a runtime)
                { const uint8_t *wnd=def_wound(g); int nn=simp_count(s);
                  for(int i=0;i<nn;i++){ int slot=simp_slot_of(s,i);
                      if(wnd[slot]!=DW_NONE) vat_layer_make_bloody(vl,slot);
                      // i maimed_legs passano alla mesh crawler, i maimed_arm al
                      // monco (a runtime). Niente stun da gestire qui: la
                      // mutilazione non emette HIT/stun (vedi defense.c apply_damage).
                      if(wnd[slot]==DW_CRAWLING)        vat_layer_set_variant(vl,slot,CRAWLER_VAR);
                      else if(wnd[slot]==DW_MAIMED_ARM) vat_layer_set_variant(vl,slot,ARM_VAR); } }
                vat_layer_update(vl,s,FIXED_DT);
                fx_update(&fx,FIXED_DT,NULL,fx_ground,NULL);   // avanza le gocce di sangue
                Uint64 t2=SDL_GetPerformanceCounter();
                sim_ms+=(double)(t1-t0)*1000.0/pf;
                lay_ms+=(double)(t2-t1)*1000.0/pf;
                acc_t-=FIXED_DT;
            }
        } else acc_t=0.0;                              // in pausa: niente debito

        // dimensione drawable corrente (resize/fullscreen/DPI). Headless (shot/
        // bench) resta inchiodata a 1280×720 → screenshot deterministici.
        if(!shot && !bench_meas) SDL_GetWindowSizeInPixels(win,&SW,&SH);
        // camera col mouse (una volta per frame, VP del frame precedente). Pan:
        // sposta il centro così il punto-ancora resta sotto il cursore. Rotate:
        // delta pixel -> azimut/elevazione (clamp per non ribaltare).
        if(drag_cam==1){ float cxw,cyw;
            if(pick_y0(vp,mouse_px,mouse_py,SW,SH,&cxw,&cyw)){ cx+=drag_anx-cxw; cz+=drag_any-cyw; } }
        else if(drag_cam==2){ az-=(mouse_px-rot_px)*0.005f; el+=(mouse_py-rot_py)*0.005f;
            if(el<0.08f)el=0.08f; if(el>1.50f)el=1.50f; rot_px=mouse_px; rot_py=mouse_py; }
        float asp=(float)SW/SH; mat4 proj,view; float ctr[3]={cx,0.9f,cz},up[3]={0,1,0};
        float eye[3]={cx+hh*cosf(el)*sinf(az),0.9f+hh*sinf(el),cz+hh*cosf(el)*cosf(az)};
        if(cam_free)m_persp(proj,45.0f*3.14159f/180.0f,asp,0.1f,500.0f);
        else m_ortho(proj,-hh*asp,hh*asp,-hh,hh,-200,400);
        m_lookat(view,eye,ctr,up); m_mul(vp,proj,view);

        Uint64 r0=SDL_GetPerformanceCounter();
        glViewport(0,0,SW,SH);glClearColor(0.12f,0.13f,0.16f,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

        // terreno glb (suolo texturizzato), sotto a tutto
        if(groundOn){ glUseProgram(progGnd);glUniformMatrix4fv(uVPgnd,1,GL_FALSE,vp);
            glUniform1i(uHasGnd, useTex && gnd.hasTex); glUniform3f(uColGnd,0.30f,0.32f,0.26f);
            glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,gnd.tex);
            glBindVertexArray(gnd.vao);glDrawElements(GL_TRIANGLES,gnd.nidx,GL_UNSIGNED_INT,0); }
        // glb#2 statici (palazzi/rocce): sempre, stesso shader. Tinta neutra se
        // privo di texture (placeholder flat finché non è texturizzato).
        if(staticsOn){ glUseProgram(progGnd);glUniformMatrix4fv(uVPgnd,1,GL_FALSE,vp);
            glUniform1i(uHasGnd, useTex && gStat.hasTex); glUniform3f(uColGnd,0.45f,0.45f,0.48f);
            glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,gStat.tex);
            glBindVertexArray(gStat.vao);glDrawElements(GL_TRIANGLES,gStat.nidx,GL_UNSIGNED_INT,0); }

        // ostacoli + suolo (statici)
        glUseProgram(progFlat);glUniformMatrix4fv(uVPflat,1,GL_FALSE,vp);
        glBindVertexArray(obVao);glDrawArrays(GL_TRIANGLES,0,obNV);
        // prop di decoro (placeholder render-only, §10 stadio 5b)
        if(prNV){ glBindVertexArray(prVao);glDrawArrays(GL_TRIANGLES,0,prNV); }
        // draggable (cassonetti/barricate, DRAG_DESIGN.md): rebuild ogni frame
        { int dNV=build_drag_mesh(s,dragBuf);
          if(dNV){ glBindVertexArray(dragVao);glBindBuffer(GL_ARRAY_BUFFER,dragVbo);
              glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)dNV*9*sizeof(float),dragBuf);
              glDrawArrays(GL_TRIANGLES,0,dNV); } }
        // torrette (pilastrini, rebuild ogni frame: le distrutte spariscono)
        { int turNV=build_turret_mesh(g,turBuf);
          if(turNV){ glBindVertexArray(turVao);glBindBuffer(GL_ARRAY_BUFFER,turVbo);
              glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)turNV*9*sizeof(float),turBuf);
              glDrawArrays(GL_TRIANGLES,0,turNV); } }
        // tracer di fuoco (linee, una per torretta che ha sparato di recente)
        { float trbuf[NT*2*9]; int tc=0;
          for(int id=0;id<def_turret_count(g);id++){ DefTurret *t=def_turret(g,id);
              if(t->tracer_ttl<=0.0f) continue;
              float ex=t->x+cosf(t->ang)*t->last_t, ez=t->y+sinf(t->ang)*t->last_t;
              float *o=trbuf+tc*9; o[0]=t->x;o[1]=0.9f+ter_z(t->x,t->y);o[2]=t->y;
              o[3]=0;o[4]=1;o[5]=0; o[6]=3;o[7]=3;o[8]=0.4f; tc++;
              o=trbuf+tc*9; o[0]=ex;o[1]=0.9f+ter_z(ex,ez);o[2]=ez;
              o[3]=0;o[4]=1;o[5]=0; o[6]=3;o[7]=3;o[8]=0.4f; tc++; }
          if(tc){ glBindVertexArray(trVao);glBindBuffer(GL_ARRAY_BUFFER,trVbo);
              glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)tc*9*sizeof(float),trbuf);
              glLineWidth(2.5f); glDrawArrays(GL_LINES,0,tc); } }

        // gib (GFX §5): pezzi balistici, un box per pezzo, mesh ogni frame
        { int ng=vat_layer_fill_gibs(vl,gibparam,GIBMAX); int gc=0;
          for(int gi=0;gi<ng;gi++){ float *p=gibparam+gi*10;
              float zb=ter_z(p[0],p[1])+p[2];
              gc=prop_box(gibmesh,gc, p[0],p[1], 0.0f,0.0f, zb, p[3],p[5],p[4],
                          cosf(p[6]),sinf(p[6]), p[7],p[8],p[9]); }
          if(gc){ glBindVertexArray(gibVao);glBindBuffer(GL_ARRAY_BUFFER,gibVbo);
              glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)gc*9*sizeof(float),gibmesh);
              glDrawArrays(GL_TRIANGLES,0,gc); } }

        // gore mesh-gibs (arti recisi + frammenti): mesh 3D texturizzate, tumble 3D
        if(nGibMesh>0){ int nm=vat_layer_fill_mesh_gibs(vl,meshgib,256);
          if(nm){ glUseProgram(mProg);glUniformMatrix4fv(uVPm,1,GL_FALSE,vp);
              glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,A[0].texD);
              glUniform1i(glGetUniformLocation(mProg,"uTex"),0);
              for(int gi=0;gi<nm;gi++){ float *p=meshgib+gi*9; int mid=(int)(p[0]+0.5f);
                  if(mid<0||mid>=nGibMesh) continue;
                  float wx=p[1], wz=p[2], wy=ter_z(p[1],p[2])+p[3];
                  mat4 mm; gib_model(mm, wx,wy,wz, p[4],p[5],p[6], p[7], p[8]*GIB_UNIT);
                  glUniformMatrix4fv(uModelm,1,GL_FALSE,mm);
                  glBindVertexArray(GM[mid].vao);
                  glDrawElements(GL_TRIANGLES,GM[mid].nidx,GL_UNSIGNED_SHORT,0); } } }

        // strutture (rebuild dallo stato vivo: celle crollate spariscono). Il pass
        // mesh-gib sopra lascia attivo mProg → ri-bind ESPLICITO di progFlat (questo
        // blocco non lo settava, ereditava lo stato dai draw flat precedenti).
        if(gStructOn){ int sv=build_struct_mesh(g,sc.cell,stBuf);
            glUseProgram(progFlat);glUniformMatrix4fv(uVPflat,1,GL_FALSE,vp);
            if(sv){ glBindVertexArray(stVao);glBindBuffer(GL_ARRAY_BUFFER,stVbo);
                glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)sv*9*sizeof(float),stBuf);
                glDrawArrays(GL_TRIANGLES,0,sv); } }

        // decal di sangue persistenti (a terra, blended): sotto orda e ombre
        { int nd=vat_layer_fill_decals(vl,decalraw,DECALMAX);
          for(int i=0;i<nd;i++){ float *r=decalraw+i*6,*o=decalinst+i*7;
              o[0]=r[0]; o[1]=ter_z(r[0],r[1])+0.03f; o[2]=r[1];
              o[3]=r[2]; o[4]=r[3]; o[5]=r[4]; o[6]=r[5]; }
          if(nd){ glUseProgram(progDc);glUniformMatrix4fv(uVPdc,1,GL_FALSE,vp);
              glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDepthMask(GL_FALSE);
              glBindVertexArray(dcVao);glBindBuffer(GL_ARRAY_BUFFER,dcInst);
              glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)nd*7*sizeof(float),decalinst);
              glDrawArraysInstanced(GL_TRIANGLE_FAN,0,SHADN,nd);
              glDepthMask(GL_TRUE);glDisable(GL_BLEND); } }

        // sagome-cadavere persistenti (sopra il sangue, sotto l'orda): quad
        // orientati col modello bakato. Blended, no depth write. CORPSE_DESIGN §10.2.
        { int nq=vat_layer_fill_corpse_decals(vl,cdecraw,CORPSEDECMAX);
          for(int i=0;i<nq;i++){ float *r=cdecraw+i*6,*o=cdecinst+i*7;
              o[0]=r[0]; o[1]=ter_z(r[0],r[1])+0.05f; o[2]=r[1]; o[3]=r[2];  // sopra la macchia (+0.05)
              o[4]=CORPSE_HALF*r[3]; o[5]=r[4]; o[6]=r[5]; }                  // half, colonna, outfit
          if(nq){ glUseProgram(cdProg);glUniformMatrix4fv(uVPcd,1,GL_FALSE,vp);
              glUniform1f(uNColsCd,(float)corpseNCols); glUniform1f(uNOutCd,(float)corpseNOut);
              glUniform1i(uNormLitCd,1);                                // relight per-pixel (anti-piattume)
              glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,corpseAlb);
              glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,corpseNrm);
              glUniform1i(glGetUniformLocation(cdProg,"uAlbedo"),0);
              glUniform1i(glGetUniformLocation(cdProg,"uNormal"),1);
              glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDepthMask(GL_FALSE);
              glBindVertexArray(cdVao);glBindBuffer(GL_ARRAY_BUFFER,cdInst);
              glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)nq*7*sizeof(float),cdecinst);
              glDrawArraysInstanced(GL_TRIANGLES,0,6,nq);
              glDepthMask(GL_TRUE);glDisable(GL_BLEND); } }

        // overlay editor (rect/poly-in-corso/cursore) sopra il terreno, flat shader.
        if(ed.active){ int ov=ed_overlay(&sc,&ed,edovl,EDOVL_MAXV);
            // cursore rosso sopra uno statico: feedback del veto di piazzamento
            if(ed.have_cursor && ter_blocked(ed.curx,ed.cury) && ov+6<=EDOVL_MAXV)
                ov=ed_push_marker(edovl,ov,ed.curx,ed.cury,0.7f, 0.95f,0.15f,0.15f);
            if(ov){ glUseProgram(progFlat);glUniformMatrix4fv(uVPflat,1,GL_FALSE,vp);
                glBindVertexArray(ovVao);glBindBuffer(GL_ARRAY_BUFFER,ovVbo);
                glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)ov*9*sizeof(float),edovl);
                glDrawArrays(GL_TRIANGLES,0,ov); } }

        int total=0;
      if(!ed.active){   // in EDIT niente orda viva: scena statica + overlay
        // ombre a terra (sotto l'orda, alla quota del terreno): blend, no depth write.
        // Per i flyer l'ombra resta a terra (terrain_z, non la quota di volo) e si
        // RIMPICCIOLISCE con l'altezza za[] → segnala visivamente quanto è alto.
        { const float *px=simp_px(s),*py=simp_py(s),*rad=simp_radius_arr(s),*za=simp_z_arr(s);
          int n=simp_count(s);
          int nsh=0; for(int i=0;i<n && nsh<MAXA;i++){
              float ax=px[i], ay=py[i], fz=za?za[i]:0.0f; float *o=shad+nsh*4;
              o[0]=ax; o[1]=ter_z(ax,ay)+0.02f; o[2]=ay;
              o[3]=rad[i]*1.35f/(1.0f+0.20f*fz); nsh++; }
          if(nsh){ glUseProgram(progSh);glUniformMatrix4fv(uVPsh,1,GL_FALSE,vp);
              glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDepthMask(GL_FALSE);
              glBindVertexArray(shVao);glBindBuffer(GL_ARRAY_BUFFER,shInst);
              glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)nsh*4*sizeof(float),shad);
              glDrawArraysInstanced(GL_TRIANGLE_FAN,0,SHADN,nsh);
              glDepthMask(GL_TRUE);glDisable(GL_BLEND); } }

        // orda VAT
        glUseProgram(prog);glUniformMatrix4fv(uVP,1,GL_FALSE,vp);
        glUniform1i(glGetUniformLocation(prog,"texPos"),0);
        glUniform1i(glGetUniformLocation(prog,"texNorm"),1);
        glUniform1i(glGetUniformLocation(prog,"texDiff"),2);

        for(int v=0;v<NVAR;v++){
            int count=vat_layer_fill_variant(vl,s,v,inst,MAXA);
            if(!count) continue; total+=count;
            // posa gli sprite sulla quota del terreno (o[1]=altezza, o[0]/o[2]=x/y)
            if(gTerOn) for(int q=0;q<count;q++) inst[q*12+1]+=ter_z(inst[q*12+0],inst[q*12+2]);
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

        // particle system (sangue): billboard istanziati, due passate (alpha poi additivo)
        { glUseProgram(pProg); glUniformMatrix4fv(uVPp,1,GL_FALSE,vp);
          glUniform1i(uHasAtl,0); glUniform1f(uAtlGrid,1.0f);
          glEnable(GL_BLEND); glDepthMask(GL_FALSE);
          glBindVertexArray(pVao);
          float right[3]={view[0],view[4],view[8]}, up[3]={view[1],view[5],view[9]};
          for(int pass=0;pass<2;pass++){
              FxBlend bm = pass==0?FX_BLEND_ALPHA:FX_BLEND_ADD;
              int pc=fx_collect(&fx,bm,right,up,pmat,pcol,pspr,FX_MAX_PARTICLES);
              if(!pc) continue;
              glBlendFunc(GL_SRC_ALPHA, bm==FX_BLEND_ADD?GL_ONE:GL_ONE_MINUS_SRC_ALPHA);
              glBindBuffer(GL_ARRAY_BUFFER,pMatVbo);glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)pc*16*sizeof(float),pmat);
              glBindBuffer(GL_ARRAY_BUFFER,pColVbo);glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)pc*4*sizeof(float),pcol);
              glBindBuffer(GL_ARRAY_BUFFER,pSprVbo);glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)pc*sizeof(float),pspr);
              glDrawArraysInstanced(GL_TRIANGLES,0,6,pc);
          }
          glBindVertexArray(0); glDepthMask(GL_TRUE); glDisable(GL_BLEND); }
      }   // fine if(!ed.active)
        glFinish();
        double ren_ms=(double)(SDL_GetPerformanceCounter()-r0)*1000.0/pf;

        // HUD: medie mobili sim/layer/render nel titolo
        acc_sim+=sim_ms; acc_lay+=lay_ms; acc_ren+=ren_ms; acc_n++;
        if(ed.active && acc_n>=30){ char t[320];
            char pinfo[64]=""; if(ed.tool==ED_PROP) snprintf(pinfo,sizeof pinfo," | prop:%s rot%.0f ([]=tipo ,.=ruota)", ed.prop_key, (double)ed.prop_rot);
            snprintf(t,sizeof t,"vat_horde EDIT — tool:%s%s | poly h%.1f cost%.0f | rect cost%.0f%s | cur(%.1f,%.1f) | g=%d goal%d spawn%d cost%d pack%d poly%d prop%d%s | TAB=play F2=save",
                ED_TOOL_NAME[ed.tool], ed.dragging?"*":"", (double)ed.poly_h,(double)ed.poly_cost,(double)ed.rect_cost, pinfo,
                (double)ed.curx,(double)ed.cury, ed.snap, sc.n_goal,sc.n_spawn,sc.n_cost,sc.n_pack,sc.n_poly,sc.n_prop,
                ed.dirty?" *":"");
            SDL_SetWindowTitle(win,t); acc_sim=acc_lay=acc_ren=0; acc_n=0; }
        else if(acc_n>=30){ char title[384]; double S=acc_sim/acc_n,L=acc_lay/acc_n,R=acc_ren/acc_n;
            char base[96]="";
            if(gCoreId>=0){ int pc=(int)(100.0f*def_struct_hp(g,gCoreId)/(def_struct_hp_max(g,gCoreId)+1e-3f));
                int po=(int)(100.0f*def_struct_hp(g,gOuterId)/(def_struct_hp_max(g,gOuterId)+1e-3f));
                snprintf(base,sizeof base, def_lost(g)?" | BASE PERSA":" | ring %d%% core %d%%", po<0?0:po, pc<0?0:pc); }
            else if(gStructOn){ int ns=def_struct_count(g),up=0; for(int q=0;q<ns;q++) if(!def_struct_collapsed(g,q)) up++;
                snprintf(base,sizeof base," | mura %d/%d", up, ns); }
            char wv[48]=""; if(dir) snprintf(wv,sizeof wv," | ondata %d budget %d",def_director_wave(dir),def_budget(g));
            snprintf(title,sizeof title,"vat_horde — %d agenti%s | kills %d crawler %d%s | sim %.2f ren %.2f ms | %.0f fps",
                     total,wv,def_kills(g),def_count_wound(g,DW_CRAWLING),base,S,R,1000.0/(S+L+R));
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
            if(gCoreId>=0) printf("  base: ring HP %.0f/%.0f%s | core HP %.0f/%.0f%s | %s\n",
                   (double)def_struct_hp(g,gOuterId),(double)def_struct_hp_max(g,gOuterId),
                   def_struct_collapsed(g,gOuterId)?" CROLLATO":"",
                   (double)def_struct_hp(g,gCoreId),(double)def_struct_hp_max(g,gCoreId),
                   def_struct_collapsed(g,gCoreId)?" CROLLATO":"", def_lost(g)?"BASE PERSA":"base regge");
            else if(gStructOn){ int ns=def_struct_count(g),up=0; for(int q=0;q<ns;q++) if(!def_struct_collapsed(g,q)) up++;
                printf("  strutture: %d/%d in piedi\n", up, ns); }
            shot_done=1; running=0; }
        SDL_GL_SwapWindow(win);
    }
    free(stBuf); free(turBuf); free(dragBuf); if(dir) def_director_destroy(dir);
    if(gTerOn) terrain_free(&gTer);
    def_destroy(g); vat_layer_destroy(vl); simp_destroy(s); scene_free(&sc);
    SDL_GL_DestroyContext(ctx);SDL_DestroyWindow(win);SDL_Quit(); return 0;
}
