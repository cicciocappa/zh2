// fxlab — laboratorio effetti visivi (FX_LAB.md). Eseguibile SEPARATO da
// vat_horde per studiare gli FX UNO ALLA VOLTA senza sim/orda/director/nav: un
// SOLO agente scriptato (fa la spola lungo un goal) reso con gli STESSI moduli
// del gioco — vat_layer.c, gli shader, i pool gib/decal/sagome-cadavere. Il
// driver è diverso (un agente al posto dell'orda), gli EFFETTI sono condivisi:
// migrare in vat_horde = accendere un flag, non riscrivere (FX_LAB principio 1).
//
//   ./fxlab [terreno.glb]            (default: blend/road_test.glb)
//
// Camera: LMB drag = pan · RMB drag = ruota · rotellina = zoom · frecce/+- idem.
// Effetti (sull'agente vivo): [ ] = cambia body · O = insanguina · H = colpo
//   (flinch) · M = mutila→crawler · G = schizzo (gib piccolo) · K = morte
//   (cadavere+decal) · B = morte esplosiva (gib grande) · R = respawn · SPACE
//   pausa · C = camera libera/ortho · T = texture on/off · ESC = esci.
// Headless: FXLAB_SHOT="<frame>" ./fxlab -> N step, screenshot fxlab_shot.bmp,
//   esce. FXLAB_CAM="cx,cz,hh,az,el" per fissare la camera.
#include <SDL3/SDL.h>
#include "vat_gl.h"
#include "vat_layer.h"
#include "sim_particles.h"
#include "terrain.h"
#include "fx_particles.h"
#include "cgltf.h"
#include "edit_pick.h"

// UI immediate-mode (nuklear, single-header C puro): pannello per scegliere
// body/outfit e lanciare gli effetti senza ricordare i tasti. Backend SDL3+GL3
// scritto qui sotto (nkgl_*). Implementazione in QUESTO TU (unico che la usa).
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#include "nuklear.h"

// I body VAT bakati (vat/assets/), STESSO elenco di vat_horde. Gli ultimi tre
// (arm, crawler, tank) sono body "di gioco": non assegnati a caso, solo via pin.
static const char *PREFIX[] = {
    "assets/zombies/zombie_man", "assets/zombies/zombie_man_obese",
    "assets/zombies/zombie_fem", "assets/zombies/zombie_fem_obese",
    "assets/zombies/zombie_child", "assets/zombies/zombie_fem_skirt",
    "assets/zombies/zombie_maimed_arm",
    "assets/zombies/zombie_maimed_legs",
    "assets/zombies/zombie_tank",
};
#define NVAR ((int)(sizeof(PREFIX)/sizeof(PREFIX[0])))
#define TANK_VAR    (NVAR-1)
#define CRAWLER_VAR (NVAR-2)
#define ARM_VAR     (NVAR-3)
#define NCOSMETIC   (NVAR-3)

#define MAXSLOT 256
typedef struct { GLuint vao, texP, texN, texD; int ni, hasTex; const VatMeta *M; } Asset;
typedef struct { GLuint vao, vbo, ebo, tex; int nidx, hasTex; } Ground;

static float inst[MAXSLOT*14];   /* 12 base + 2 tumble (pitch,roll) */

// terreno (heightmap render-only) + offset mondo: il footprint del glb può stare
// a coord negative (origin del .zhm); trasliamo tutto in coord >=0 per la sim e
// campioniamo la quota riaggiungendo l'origine.
static Terrain gTer; static int gTerOn=0;
static float OFX=0, OFY=0;   // = origin del .zhm (sx_mondo = sx_sim + OFX)
static float ter_z(float x, float y){ return gTerOn ? terrain_z(&gTer, x+OFX, y+OFY) : 0.0f; }
// ground callback for the particle system: particle horizontal coords are
// (x, z) = (world x, sim y), the floor height there is ter_z(x, z).
static float fx_ground(float x, float z, void *ud){ (void)ud; return ter_z(x,z); }

// blood splatter preset: a fine omnidirectional burst of dark-red droplets with
// real gravity that arc and stick to the ground (ground_stop). Versatile-by-data
// FX_LAB step 2: fire/smoke/explosions are other FxEmitterDef presets.
static const FxEmitterDef BLOOD_DEF = {
    .count=28, .shape=FX_EMIT_POINT,
    .spawn_radius=0.05f, .spawn_box_z=0.05f,
    .spawn_offset_y_min=-0.05f, .spawn_offset_y_max=0.10f,
    .speed_xy_min=1.2f, .speed_xy_max=4.0f,
    .speed_y_min=1.2f,  .speed_y_max=4.5f,
    .gravity=9.81f, .drag=0.6f,
    .lifetime_min=0.5f, .lifetime_max=1.2f,
    .start_scale_min=0.05f, .start_scale_max=0.11f,
    .end_scale_min=0.02f,   .end_scale_max=0.05f,
    .start_color={0.50f,0.02f,0.02f,1.0f}, .end_color={0.22f,0.0f,0.0f,0.55f},
    .color_variants={ {0.55f,0.03f,0.03f,1.0f},{0.42f,0.0f,0.0f,1.0f},{0.60f,0.08f,0.04f,1.0f} },
    .color_variant_count=3,
    .sprite_first=-1, .sprite_last=-1,
    .wind_scale=0.3f, .ground_stop=true, .blend=FX_BLEND_ALPHA,
    .rate=20.0f,
};

// Schizzo ABBONDANTE per l'esplosione totale del corpo: più particelle, getto più
// ampio e veloce, gocce un filo più grosse e durature di BLOOD_DEF.
static const FxEmitterDef BLOOD_BURST_DEF = {
    .count=90, .shape=FX_EMIT_POINT,
    .spawn_radius=0.10f, .spawn_box_z=0.10f,
    .spawn_offset_y_min=-0.05f, .spawn_offset_y_max=0.20f,
    .speed_xy_min=1.5f, .speed_xy_max=6.0f,
    .speed_y_min=2.0f,  .speed_y_max=7.0f,
    .gravity=9.81f, .drag=0.55f,
    .lifetime_min=0.6f, .lifetime_max=1.6f,
    .start_scale_min=0.06f, .start_scale_max=0.16f,
    .end_scale_min=0.02f,   .end_scale_max=0.06f,
    .start_color={0.50f,0.02f,0.02f,1.0f}, .end_color={0.20f,0.0f,0.0f,0.5f},
    .color_variants={ {0.55f,0.03f,0.03f,1.0f},{0.42f,0.0f,0.0f,1.0f},{0.60f,0.08f,0.04f,1.0f} },
    .color_variant_count=3,
    .sprite_first=-1, .sprite_last=-1,
    .wind_scale=0.3f, .ground_stop=true, .blend=FX_BLEND_ALPHA,
    .rate=20.0f,
};

// --- glTF ground loader (copia di vat_horde.load_ground_glb) con TRASLAZIONE
// orizzontale: sposta il footprint del suolo in coord >=0 (offx,offy = -origin).
static GLuint ground_load_tex(cgltf_data *data, cgltf_primitive *prim, const char *dir){
    (void)data;
    if(!prim->material || !prim->material->has_pbr_metallic_roughness) return 0;
    cgltf_texture *t = prim->material->pbr_metallic_roughness.base_color_texture.texture;
    if(!t || !t->image) return 0;
    cgltf_image *img = t->image;
    if(img->uri && strncmp(img->uri,"data:",5)!=0){
        char path[512]; snprintf(path,sizeof path,"%s%s",dir,img->uri);
        return vg_tex_png(path);
    }
    if(img->buffer_view){
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
static int load_ground_glb(const char *path, Ground *G, float offx, float offy){
    memset(G,0,sizeof *G);
    cgltf_options opt={0}; cgltf_data *data=NULL;
    if(cgltf_parse_file(&opt,path,&data)!=cgltf_result_success){ fprintf(stderr,"ground: parse fail %s\n",path); return -1; }
    if(cgltf_load_buffers(&opt,data,path)!=cgltf_result_success){ fprintf(stderr,"ground: load buffers fail\n"); cgltf_free(data); return -1; }
    char dir[512]; snprintf(dir,sizeof dir,"%s",path);
    char *slash=strrchr(dir,'/'); if(slash) slash[1]='\0'; else dir[0]='\0';
    size_t totV=0, totI=0;
    for(size_t n=0;n<data->nodes_count;n++){ cgltf_node *nd=&data->nodes[n]; if(!nd->mesh) continue;
        for(size_t p=0;p<nd->mesh->primitives_count;p++){ cgltf_primitive *pr=&nd->mesh->primitives[p];
            if(pr->type!=cgltf_primitive_type_triangles||!pr->indices) continue;
            cgltf_accessor *pos=NULL; for(size_t a=0;a<pr->attributes_count;a++) if(pr->attributes[a].type==cgltf_attribute_type_position) pos=pr->attributes[a].data;
            if(!pos) continue; totV+=pos->count; totI+=pr->indices->count; } }
    if(totV==0||totI==0){ fprintf(stderr,"ground: no triangles in %s\n",path); cgltf_free(data); return -1; }
    float *verts=malloc(totV*8*sizeof(float)); unsigned *idx=malloc(totI*sizeof(unsigned));
    GLuint tex=0; size_t vbase=0, ibase=0;
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
                float wx=M[0]*P[0]+M[4]*P[1]+M[8]*P[2]+M[12];
                float wy=M[1]*P[0]+M[5]*P[1]+M[9]*P[2]+M[13];
                float wz=M[2]*P[0]+M[6]*P[1]+M[10]*P[2]+M[14];
                float nx=M[0]*N[0]+M[4]*N[1]+M[8]*N[2];
                float ny=M[1]*N[0]+M[5]*N[1]+M[9]*N[2];
                float nz=M[2]*N[0]+M[6]*N[1]+M[10]*N[2];
                float *o=verts+(vbase+v)*8;
                o[0]=wx+offx; o[1]=wy; o[2]=-wz+offy; o[3]=nx; o[4]=ny; o[5]=-nz; o[6]=T[0]; o[7]=T[1];
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

// --- gore mesh-gibs: carica le mesh di assets/models/gibs.glb (arm + 2 frammenti) in un
// VAO ciascuna (pos/norm/uv, attrib 0/1/2), CENTRATE sul centroide del bbox così
// il tumble ruota attorno al centro. Indicizzate per ORDINE DI NODO = mesh_id
// (0=arm, 1=gib1, 2=gib2, 3=leg, 4=head, 5/6=torso). UV sul diffuse del corpo.
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
        out[got].vao=vao; out[got].nidx=(int)ni;
        printf("gib mesh[%d] %s: %zu verts %zu tris\n",got,nd->name?nd->name:"?",nv,ni/3);
        got++;
    }
    cgltf_free(data); return got;
}

// column-major model matrix: T(world) * rot(axis,angle) * uniform scale.
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

// box per i gib (copia di vat_horde.prop_box): top + 4 pareti, 30 vertici.
#define GIB_VERTS_EACH 30
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
        PP(wx[k],y1,wz[k],nx,0,nz); PP(wx[k1],y1,wz[k1],nx,0,nz); PP(wx[k1],y0,wz[k1],nx,0,nz);
        PP(wx[k],y1,wz[k],nx,0,nz); PP(wx[k1],y0,wz[k1],nx,0,nz); PP(wx[k],y0,wz[k],nx,0,nz);
    }
#undef PP
    return c;
}

// goal: una banda orizzontale di celle al bordo (alto o basso). Pulisce le due
// bande prima di settare quella attiva -> la spola.
static void set_goal_band(SimP *s, int gw, int gh, int toward_top){
    int yb=2, yt=gh-3;
    for(int x=0;x<gw;x++){ simp_set_goal(s,x,yb,false); simp_set_goal(s,x,yt,false); }
    int gy = toward_top ? yt : yb;
    for(int x=0;x<gw;x++) simp_set_goal(s,x,gy,true);
}

// ============================ backend nuklear (SDL3 + GL3) ==================
// Adattamento del demo ufficiale nuklear_sdl_gl3 al nostro contesto SDL3/GL3.3.
#define NKGL_MAX_VTX (256*1024)
#define NKGL_MAX_IDX (128*1024)
struct nk_vertex { float pos[2]; float uv[2]; nk_byte col[4]; };
typedef struct {
    struct nk_context ctx;
    struct nk_font_atlas atlas;
    struct nk_buffer cmds;
    struct nk_draw_null_texture tex_null;
    GLuint prog, vbo, ebo, vao, font_tex;
    GLint u_tex, u_proj, a_pos, a_uv, a_col;
} NkGL;

static GLuint nkgl_compile(const char *vs_src, const char *fs_src){
    GLint ok; char log[1024];
    GLuint v=glCreateShader(GL_VERTEX_SHADER); glShaderSource(v,1,&vs_src,0); glCompileShader(v);
    glGetShaderiv(v,GL_COMPILE_STATUS,&ok); if(!ok){glGetShaderInfoLog(v,1024,0,log);fprintf(stderr,"NK VS:%s\n",log);}
    GLuint f=glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(f,1,&fs_src,0); glCompileShader(f);
    glGetShaderiv(f,GL_COMPILE_STATUS,&ok); if(!ok){glGetShaderInfoLog(f,1024,0,log);fprintf(stderr,"NK FS:%s\n",log);}
    GLuint p=glCreateProgram(); glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p);
    glGetProgramiv(p,GL_LINK_STATUS,&ok); if(!ok){glGetProgramInfoLog(p,1024,0,log);fprintf(stderr,"NK LINK:%s\n",log);}
    glDeleteShader(v); glDeleteShader(f); return p;
}
static void nkgl_init(NkGL *n){
    static const char *VS=
        "#version 330\n uniform mat4 ProjMtx;\n in vec2 Position;\n in vec2 TexCoord;\n in vec4 Color;\n"
        "out vec2 Frag_UV;\n out vec4 Frag_Color;\n"
        "void main(){ Frag_UV=TexCoord; Frag_Color=Color; gl_Position=ProjMtx*vec4(Position.xy,0,1); }";
    static const char *FS=
        "#version 330\n precision mediump float;\n uniform sampler2D Texture;\n in vec2 Frag_UV;\n in vec4 Frag_Color;\n"
        "out vec4 Out_Color;\n void main(){ Out_Color = Frag_Color * texture(Texture, Frag_UV.st); }";
    n->prog=nkgl_compile(VS,FS);
    n->u_tex=glGetUniformLocation(n->prog,"Texture");
    n->u_proj=glGetUniformLocation(n->prog,"ProjMtx");
    n->a_pos=glGetAttribLocation(n->prog,"Position");
    n->a_uv =glGetAttribLocation(n->prog,"TexCoord");
    n->a_col=glGetAttribLocation(n->prog,"Color");
    glGenBuffers(1,&n->vbo); glGenBuffers(1,&n->ebo); glGenVertexArrays(1,&n->vao);
    glBindVertexArray(n->vao);
    glBindBuffer(GL_ARRAY_BUFFER,n->vbo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,n->ebo);
    GLsizei st=sizeof(struct nk_vertex);
    glEnableVertexAttribArray(n->a_pos); glVertexAttribPointer(n->a_pos,2,GL_FLOAT,GL_FALSE,st,(void*)offsetof(struct nk_vertex,pos));
    glEnableVertexAttribArray(n->a_uv);  glVertexAttribPointer(n->a_uv, 2,GL_FLOAT,GL_FALSE,st,(void*)offsetof(struct nk_vertex,uv));
    glEnableVertexAttribArray(n->a_col); glVertexAttribPointer(n->a_col,4,GL_UNSIGNED_BYTE,GL_TRUE,st,(void*)offsetof(struct nk_vertex,col));
    glBindVertexArray(0);
    // font atlante di default (ProggyClean, dentro nuklear.h)
    nk_font_atlas_init_default(&n->atlas);
    nk_font_atlas_begin(&n->atlas);
    struct nk_font *font=nk_font_atlas_add_default(&n->atlas,14.0f,0);
    int fw,fh; const void *img=nk_font_atlas_bake(&n->atlas,&fw,&fh,NK_FONT_ATLAS_RGBA32);
    glGenTextures(1,&n->font_tex); glBindTexture(GL_TEXTURE_2D,n->font_tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,fw,fh,0,GL_RGBA,GL_UNSIGNED_BYTE,img);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    nk_font_atlas_end(&n->atlas, nk_handle_id((int)n->font_tex), &n->tex_null);
    nk_init_default(&n->ctx,&font->handle);
    nk_buffer_init_default(&n->cmds);
}
static void nkgl_event(NkGL *n, const SDL_Event *e){
    struct nk_context *c=&n->ctx;
    if(e->type==SDL_EVENT_KEY_DOWN||e->type==SDL_EVENT_KEY_UP){
        int d=(e->type==SDL_EVENT_KEY_DOWN); SDL_Keycode k=e->key.key;
        if(k==SDLK_LSHIFT||k==SDLK_RSHIFT) nk_input_key(c,NK_KEY_SHIFT,d);
        else if(k==SDLK_DELETE) nk_input_key(c,NK_KEY_DEL,d);
        else if(k==SDLK_RETURN) nk_input_key(c,NK_KEY_ENTER,d);
        else if(k==SDLK_BACKSPACE) nk_input_key(c,NK_KEY_BACKSPACE,d);
        else if(k==SDLK_LEFT) nk_input_key(c,NK_KEY_LEFT,d);
        else if(k==SDLK_RIGHT) nk_input_key(c,NK_KEY_RIGHT,d);
        else if(k==SDLK_UP) nk_input_key(c,NK_KEY_UP,d);
        else if(k==SDLK_DOWN) nk_input_key(c,NK_KEY_DOWN,d);
    } else if(e->type==SDL_EVENT_MOUSE_BUTTON_DOWN||e->type==SDL_EVENT_MOUSE_BUTTON_UP){
        int d=(e->type==SDL_EVENT_MOUSE_BUTTON_DOWN); int x=(int)e->button.x,y=(int)e->button.y;
        enum nk_buttons b = e->button.button==SDL_BUTTON_RIGHT?NK_BUTTON_RIGHT:
                            e->button.button==SDL_BUTTON_MIDDLE?NK_BUTTON_MIDDLE:NK_BUTTON_LEFT;
        nk_input_button(c,b,x,y,d);
    } else if(e->type==SDL_EVENT_MOUSE_MOTION){
        nk_input_motion(c,(int)e->motion.x,(int)e->motion.y);
    } else if(e->type==SDL_EVENT_MOUSE_WHEEL){
        nk_input_scroll(c,nk_vec2(0,(float)e->wheel.y));
    }
}
static void nkgl_render(NkGL *n, int W, int H){
    GLfloat ortho[16]={ 2.0f/W,0,0,0,  0,-2.0f/H,0,0,  0,0,-1,0,  -1,1,0,1 };
    glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE); glDisable(GL_DEPTH_TEST); glEnable(GL_SCISSOR_TEST);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(n->prog); glUniform1i(n->u_tex,0); glUniformMatrix4fv(n->u_proj,1,GL_FALSE,ortho);
    glViewport(0,0,W,H);
    glBindVertexArray(n->vao);
    glBindBuffer(GL_ARRAY_BUFFER,n->vbo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,n->ebo);
    glBufferData(GL_ARRAY_BUFFER,NKGL_MAX_VTX,NULL,GL_STREAM_DRAW);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,NKGL_MAX_IDX,NULL,GL_STREAM_DRAW);
    void *vtx=glMapBuffer(GL_ARRAY_BUFFER,GL_WRITE_ONLY);
    void *idx=glMapBuffer(GL_ELEMENT_ARRAY_BUFFER,GL_WRITE_ONLY);
    {
        static const struct nk_draw_vertex_layout_element layout[]={
            {NK_VERTEX_POSITION,NK_FORMAT_FLOAT,offsetof(struct nk_vertex,pos)},
            {NK_VERTEX_TEXCOORD,NK_FORMAT_FLOAT,offsetof(struct nk_vertex,uv)},
            {NK_VERTEX_COLOR,NK_FORMAT_R8G8B8A8,offsetof(struct nk_vertex,col)},
            {NK_VERTEX_LAYOUT_END}
        };
        struct nk_convert_config cfg; memset(&cfg,0,sizeof cfg);
        cfg.vertex_layout=layout; cfg.vertex_size=sizeof(struct nk_vertex);
        cfg.vertex_alignment=NK_ALIGNOF(struct nk_vertex); cfg.tex_null=n->tex_null;
        cfg.circle_segment_count=22; cfg.curve_segment_count=22; cfg.arc_segment_count=22;
        cfg.global_alpha=1.0f; cfg.shape_AA=NK_ANTI_ALIASING_ON; cfg.line_AA=NK_ANTI_ALIASING_ON;
        struct nk_buffer vbuf,ebuf;
        nk_buffer_init_fixed(&vbuf,vtx,NKGL_MAX_VTX); nk_buffer_init_fixed(&ebuf,idx,NKGL_MAX_IDX);
        nk_convert(&n->ctx,&n->cmds,&vbuf,&ebuf,&cfg);
    }
    glUnmapBuffer(GL_ARRAY_BUFFER); glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
    const struct nk_draw_command *cmd; const nk_draw_index *off=NULL;
    nk_draw_foreach(cmd,&n->ctx,&n->cmds){
        if(!cmd->elem_count) continue;
        glBindTexture(GL_TEXTURE_2D,(GLuint)cmd->texture.id);
        glScissor((GLint)cmd->clip_rect.x,(GLint)(H-(cmd->clip_rect.y+cmd->clip_rect.h)),
                  (GLint)cmd->clip_rect.w,(GLint)cmd->clip_rect.h);
        glDrawElements(GL_TRIANGLES,(GLsizei)cmd->elem_count,GL_UNSIGNED_SHORT,off);
        off+=cmd->elem_count;
    }
    nk_clear(&n->ctx); nk_buffer_clear(&n->cmds);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glUseProgram(0); glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
}
// ===========================================================================

int main(int argc, char **argv){
    const char *glb = argc>1 ? argv[1] : "assets/models/road_test.glb";

    // heightmap a fianco del glb (stesso path, .zhm). CPU-only: caricata prima
    // di GL. Definisce footprint mondo + offset coord>=0.
    float Wm=16.0f, Hm=16.0f;
    { char zhm[256]; snprintf(zhm,sizeof zhm,"%s",glb);
      char *dot=strrchr(zhm,'.'); if(dot) strcpy(dot,".zhm"); else strncat(zhm,".zhm",sizeof zhm-strlen(zhm)-1);
      if(terrain_load(zhm,&gTer)==0){ gTerOn=1;
          OFX=gTer.origin_x; OFY=gTer.origin_y;
          Wm=(gTer.w-1)/gTer.px_per_m; Hm=(gTer.h-1)/gTer.px_per_m;
          printf("terreno: %s (%dx%d @ %.1f px/m) footprint %.1fx%.1f m, origin (%.2f,%.2f)\n",
                 zhm,gTer.w,gTer.h,(double)gTer.px_per_m,(double)Wm,(double)Hm,(double)OFX,(double)OFY);
      } else printf("terreno: %s mancante -> suolo piatto z=0 (%.0fx%.0f m)\n",zhm,(double)Wm,(double)Hm);
    }

    // griglia nav: copre il footprint, coord >=0 (cell 0.5 m).
    const float cell=0.5f;
    int gw=(int)ceilf(Wm/cell)+1, gh=(int)ceilf(Hm/cell)+1;
    SimP *s=simp_create(gw,gh,cell,MAXSLOT);
    if(!s){ fprintf(stderr,"simp_create fail\n"); return 1; }

    // layer VAT multi-body (stessi asset di vat_horde).
    char metas[NVAR][256]; const char *metap[NVAR];
    for(int v=0;v<NVAR;v++){ snprintf(metas[v],256,"%s_meta.txt",PREFIX[v]); metap[v]=metas[v]; }
    VatLayer *vl=vat_layer_create_multi(metap,NVAR,MAXSLOT);
    if(vat_layer_nvariants(vl)!=NVAR){ fprintf(stderr,"VAT_MAX_VARIANTS < NVAR (%d): alza il limite\n",NVAR); return 1; }
    vat_layer_set_random_count(vl,NCOSMETIC);
    vat_layer_set_variant_scale(vl,TANK_VAR, 1.90f/1.8f, 2.05f/1.8f);

    // l'agente scriptato: parte in basso, goal in alto, fa la spola.
    int toward_top=1; set_goal_band(s,gw,gh,toward_top);
    int idx=simp_spawn(s, Wm*0.5f, 1.2f);
    SimPHandle ah = simp_handle_of(s,idx);
    int cur_var=0;
    float respawn_t=0;   // >0: agente morto, respawn quando scade

    int SW=1280,SH=720;
    if(!SDL_Init(SDL_INIT_VIDEO)){fprintf(stderr,"SDL:%s\n",SDL_GetError());return 1;}
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,24);
    SDL_Window*win=SDL_CreateWindow("fxlab",SW,SH,SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE);
    SDL_GLContext ctx=SDL_GL_CreateContext(win);
    if(!ctx||!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)){fprintf(stderr,"GL init fail\n");return 1;}
    printf("GL %s\n",glGetString(GL_VERSION));

    GLuint prog=vg_shader("assets/shaders/vat.vs","assets/shaders/vat.fs");
    GLint uVP=glGetUniformLocation(prog,"uVP"),uTS=glGetUniformLocation(prog,"texSize"),
          uRPF=glGetUniformLocation(prog,"rowsPerFrame"),uHas=glGetUniformLocation(prog,"uHasTex");
    GLuint bi; glGenBuffers(1,&bi);glBindBuffer(GL_ARRAY_BUFFER,bi);
    glBufferData(GL_ARRAY_BUFFER,sizeof(inst),NULL,GL_DYNAMIC_DRAW);

    // terreno (suolo glb texturizzato), traslato in coord >=0.
    Ground gnd; int groundOn=0; GLuint progGnd=0; GLint uVPgnd=0,uHasGnd=0,uColGnd=0;
    if(load_ground_glb(glb,&gnd,-OFX,-OFY)==0){ groundOn=1;
        progGnd=vg_shader("assets/shaders/ground.vs","assets/shaders/ground.fs");
        uVPgnd=glGetUniformLocation(progGnd,"uVP");
        uHasGnd=glGetUniformLocation(progGnd,"uHasTex");
        uColGnd=glGetUniformLocation(progGnd,"uColor");
        glUseProgram(progGnd); glUniform1i(glGetUniformLocation(progGnd,"uTex"),0); }

    // disco unitario (ombre + decal di sangue): fan a 16 segmenti.
#define SHADN 18
    float disc[SHADN*2]; disc[0]=0; disc[1]=0;
    for(int i=0;i<SHADN-1;i++){ float a=(float)i*(6.2831853f/16.0f); disc[(i+1)*2]=cosf(a); disc[(i+1)*2+1]=sinf(a); }
    GLuint progSh=vg_shader("assets/shaders/shadow.vs","assets/shaders/shadow.fs"); GLint uVPsh=glGetUniformLocation(progSh,"uVP");
    GLuint shVao,shDisc,shInst; glGenVertexArrays(1,&shVao);glBindVertexArray(shVao);
    glGenBuffers(1,&shDisc);glBindBuffer(GL_ARRAY_BUFFER,shDisc);
    glBufferData(GL_ARRAY_BUFFER,sizeof disc,disc,GL_STATIC_DRAW);
    glVertexAttribPointer(0,2,GL_FLOAT,0,2*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glGenBuffers(1,&shInst);glBindBuffer(GL_ARRAY_BUFFER,shInst);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)MAXSLOT*4*sizeof(float),NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(1,4,GL_FLOAT,0,4*sizeof(float),(void*)0);glEnableVertexAttribArray(1);glVertexAttribDivisor(1,1);
    glBindVertexArray(0);

    // decal di sangue persistenti (riusa la geom del disco).
#define DECALMAX 4096
    static float decalraw[DECALMAX*6], decalinst[DECALMAX*7];
    GLuint progDc=vg_shader("assets/shaders/decal.vs","assets/shaders/decal.fs"); GLint uVPdc=glGetUniformLocation(progDc,"uVP");
    GLuint dcVao,dcInst; glGenVertexArrays(1,&dcVao);glBindVertexArray(dcVao);
    glBindBuffer(GL_ARRAY_BUFFER,shDisc);
    glVertexAttribPointer(0,2,GL_FLOAT,0,2*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glGenBuffers(1,&dcInst);glBindBuffer(GL_ARRAY_BUFFER,dcInst);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)DECALMAX*7*sizeof(float),NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(1,4,GL_FLOAT,0,7*sizeof(float),(void*)0);glEnableVertexAttribArray(1);glVertexAttribDivisor(1,1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,7*sizeof(float),(void*)(4*sizeof(float)));glEnableVertexAttribArray(2);glVertexAttribDivisor(2,1);
    glBindVertexArray(0);

    // sagome-cadavere persistenti (quad orientati, atlante bakato init-time).
#define CORPSEDECMAX 1024
#define CORPSE_CELL  256
#define CORPSE_HALF  1.15f
    static float cdecraw[CORPSEDECMAX*6], cdecinst[CORPSEDECMAX*7];
    static const float cquad[12]={-1,-1, 1,-1, 1,1, -1,-1, 1,1, -1,1};
    GLuint cdProg=vg_shader("assets/shaders/corpse_decal.vs","assets/shaders/corpse_decal.fs");
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
    int normal_lit=0;   // toggle `P`: relighting per-pixel della sagoma (anti-piattume)
    { const char*ne=getenv("FXLAB_NORMAL"); if(ne) normal_lit=atoi(ne); }

    // gib (box balistici), flat shader.
#define GIBMAX 512
    static float gibparam[GIBMAX*10];
    float *gibmesh=malloc((size_t)GIBMAX*GIB_VERTS_EACH*9*sizeof(float));
    GLuint progFlat=vg_shader("assets/shaders/flat.vs","assets/shaders/flat.fs"); GLint uVPflat=glGetUniformLocation(progFlat,"uVP");
    GLuint gibVao,gibVbo; glGenVertexArrays(1,&gibVao);glBindVertexArray(gibVao);
    glGenBuffers(1,&gibVbo);glBindBuffer(GL_ARRAY_BUFFER,gibVbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)GIBMAX*GIB_VERTS_EACH*9*sizeof(float),NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,9*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,0,9*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,9*sizeof(float),(void*)(6*sizeof(float)));glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    // particle system (fx_particles): billboard istanziati, blob procedurali.
    FxParticles fx; fx_init(&fx, 0xC0FFEEu);
    static float pmat[FX_MAX_PARTICLES*16], pcol[FX_MAX_PARTICLES*4], pspr[FX_MAX_PARTICLES];
    static const float pquad[18]={-0.5f,-0.5f,0, 0.5f,-0.5f,0, 0.5f,0.5f,0,
                                  -0.5f,-0.5f,0, 0.5f,0.5f,0, -0.5f,0.5f,0};
    GLuint pProg=vg_shader("assets/shaders/particle.vs","assets/shaders/particle.fs");
    GLint uVPp=glGetUniformLocation(pProg,"uVP"), uHasAtl=glGetUniformLocation(pProg,"uHasAtlas"),
          uAtlGrid=glGetUniformLocation(pProg,"uAtlasGrid");
    GLuint pVao,pQuadVbo,pMatVbo,pColVbo,pSprVbo;
    glGenVertexArrays(1,&pVao); glBindVertexArray(pVao);
    glGenBuffers(1,&pQuadVbo); glBindBuffer(GL_ARRAY_BUFFER,pQuadVbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof pquad,pquad,GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,3*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glGenBuffers(1,&pMatVbo); glBindBuffer(GL_ARRAY_BUFFER,pMatVbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof pmat,NULL,GL_DYNAMIC_DRAW);
    for(int c=0;c<4;c++){ glVertexAttribPointer(1+c,4,GL_FLOAT,0,16*sizeof(float),(void*)(c*4*sizeof(float)));
        glEnableVertexAttribArray(1+c); glVertexAttribDivisor(1+c,1); }
    glGenBuffers(1,&pColVbo); glBindBuffer(GL_ARRAY_BUFFER,pColVbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof pcol,NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(5,4,GL_FLOAT,0,4*sizeof(float),(void*)0);glEnableVertexAttribArray(5);glVertexAttribDivisor(5,1);
    glGenBuffers(1,&pSprVbo); glBindBuffer(GL_ARRAY_BUFFER,pSprVbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof pspr,NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(6,1,GL_FLOAT,0,sizeof(float),(void*)0);glEnableVertexAttribArray(6);glVertexAttribDivisor(6,1);
    glBindVertexArray(0);

    // un Asset (VAO+mesh+texture VAT) per variante.
    Asset A[NVAR];
    for(int v=0;v<NVAR;v++){
        const VatMeta *M=vat_layer_meta_variant(vl,v); A[v].M=M;
        char pos[256],norm[256],mesh[256],diff[256];
        snprintf(pos,256,"%s_pos.raw",PREFIX[v]);snprintf(norm,256,"%s_norm.raw",PREFIX[v]);
        snprintf(mesh,256,"%s_mesh.bin",PREFIX[v]);snprintf(diff,256,"%s_diffuse.png",PREFIX[v]);
        FILE*mf=fopen(mesh,"rb"); if(!mf){fprintf(stderr,"no mesh %s\n",mesh);return 1;}
        int nv,ni; if(fread(&nv,4,1,mf)){}if(fread(&ni,4,1,mf)){}
        float*verts=malloc(nv*3*4),*uvs=malloc(nv*2*4); unsigned short*idxb=malloc(ni*2);
        if(fread(verts,4,nv*3,mf)){}if(fread(uvs,4,nv*2,mf)){}if(fread(idxb,2,ni,mf)){}fclose(mf);
        A[v].ni=ni;
        A[v].texP=vg_tex_raw(pos,M->texW,M->texH);
        A[v].texN=vg_tex_raw(norm,M->texW,M->texH);
        A[v].texD=vg_tex_png(diff); A[v].hasTex=A[v].texD!=0;
        GLuint vao,bp,bu,eb; glGenVertexArrays(1,&vao);glBindVertexArray(vao);
        glGenBuffers(1,&bp);glBindBuffer(GL_ARRAY_BUFFER,bp);glBufferData(GL_ARRAY_BUFFER,nv*3*4,verts,GL_STATIC_DRAW);
        glVertexAttribPointer(0,3,GL_FLOAT,0,0,0);glEnableVertexAttribArray(0);
        glGenBuffers(1,&bu);glBindBuffer(GL_ARRAY_BUFFER,bu);glBufferData(GL_ARRAY_BUFFER,nv*2*4,uvs,GL_STATIC_DRAW);
        glVertexAttribPointer(1,2,GL_FLOAT,0,0,0);glEnableVertexAttribArray(1);
        glGenBuffers(1,&eb);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,eb);glBufferData(GL_ELEMENT_ARRAY_BUFFER,ni*2,idxb,GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER,bi);
        for(int i=0;i<3;i++){glEnableVertexAttribArray(2+i);
            glVertexAttribPointer(2+i,4,GL_FLOAT,0,14*sizeof(float),(void*)(i*4*sizeof(float)));glVertexAttribDivisor(2+i,1);}
        glEnableVertexAttribArray(5);      // tumble in volo (pitch, roll)
        glVertexAttribPointer(5,2,GL_FLOAT,0,14*sizeof(float),(void*)(12*sizeof(float)));glVertexAttribDivisor(5,1);
        glBindVertexArray(0); A[v].vao=vao;
        free(verts);free(uvs);free(idxb);
    }
    glEnable(GL_DEPTH_TEST);

    // gore mesh-gibs: VAO delle 3 mesh + shader mesh statica texturizzata. La
    // texture è il diffuse di zombie_man (A[0]), su cui sono mappate le UV del glb.
    GibMesh GM[8]={0}; int nGibMesh=load_gib_meshes("assets/models/gibs.glb",GM,8);
    const float GIB_UNIT=0.01f;   // unità Blender -> metri (braccio ~0.53 m); tunabile
    GLuint mProg=vg_shader("assets/shaders/mesh.vs","assets/shaders/mesh.fs");
    GLint uVPm=glGetUniformLocation(mProg,"uVP"), uModelm=glGetUniformLocation(mProg,"uModel");
    static float meshgib[256*9];

    // BAKE atlante sagome-cadavere (copia di vat_horde): ortho top-down dell'ultimo
    // frame della posa di morte. ALBEDO = griglia (colonna=var*posa × riga=outfit);
    // NORMAL = una sola riga (outfit-indipendente). Relight per-pixel anti-piattume
    // in corpse_decal.fs. Colonna = v*NPOSE+p; posa 0='dying', 1='death' (devono
    // combaciare con vat_layer_die, le due falle a terra differiscono di ~90°).
    GLuint bakeProg=vg_shader("assets/shaders/vat.vs","assets/shaders/corpsebake.fs");
    GLint uVPb=glGetUniformLocation(bakeProg,"uVP"),uTSb=glGetUniformLocation(bakeProg,"texSize"),
          uRPFb=glGetUniformLocation(bakeProg,"rowsPerFrame"),uHasb=glGetUniformLocation(bakeProg,"uHasTex"),
          uModeb=glGetUniformLocation(bakeProg,"uMode");
    {   int Wc=CORPSE_CELL*corpseNCols, Hg=CORPSE_CELL*corpseNOut;   // albedo grid
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
        // due passate: 0 = ALBEDO (per outfit, su corpseAlb griglia), 1 = NORMAL
        // (riga unica, su corpseNrm). Frame della posa = funzione di (v,p) soltanto.
        for(int pass=0;pass<2;pass++){
            GLuint dst = pass?corpseNrm:corpseAlb; int Hh = pass?CORPSE_CELL:Hg;
            int nout = pass?1:corpseNOut;                 // normal: una sola riga
            GLuint fbo,depth; glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,dst,0);
            glGenRenderbuffers(1,&depth);glBindRenderbuffer(GL_RENDERBUFFER,depth);
            glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,Wc,Hh);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth);
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
                    if(fr<0) fr=0;   // posa mancante (es. crawler): cella inerte
                    int col=v*VAT_CORPSE_NPOSE+p;
                    for(int o=0;o<nout;o++){
                        // riga o = outfit base; bake con la versione INSANGUINATA (o+16)
                        float one[14]={0,0,0, 0, 1.0f, (float)fr,(float)fr,0, (float)(o+16), 0.55f,0.5f,0.5f, 0,0};
                        glViewport(col*CORPSE_CELL,o*CORPSE_CELL,CORPSE_CELL,CORPSE_CELL);
                        glBindBuffer(GL_ARRAY_BUFFER,bi);glBufferSubData(GL_ARRAY_BUFFER,0,14*sizeof(float),one);
                        glDrawElementsInstanced(GL_TRIANGLES,A[v].ni,GL_UNSIGNED_SHORT,0,1);
                    }
                }
            }
            glBindFramebuffer(GL_FRAMEBUFFER,0);
            glDeleteRenderbuffers(1,&depth);glDeleteFramebuffers(1,&fbo);
        }
    }

    // UI immediate-mode (nuklear): pannello body/outfit/effetti.
    NkGL nk; nkgl_init(&nk);
    static const char *BODY_NAME[NVAR]={"man","man obese","fem","fem obese","child",
        "fem skirt","monco braccio","crawler","tank"};
    int ui_outfit=0, ui_bloody=0;   // outfit base (0..13) + flag insanguinato
    int ui_force_clip=-1, anim_body=-1;  // override clip locomozione (-1=auto) + body tracciato

    // camera ortho 3/4 (come vat_horde), centrata sul footprint.
    float cx=Wm*0.5f, cz=Hm*0.5f, hh=(Wm>Hm?Wm:Hm)*0.6f, az=0.7f, el=0.55f;
    int cam_free=0,paused=0,useTex=1;
    { const char*cs=getenv("FXLAB_CAM"); if(cs) sscanf(cs,"%f,%f,%f,%f,%f",&cx,&cz,&hh,&az,&el); }
    const char *shot=getenv("FXLAB_SHOT"); int shot_frames=shot?atoi(shot):0; if(shot&&shot_frames<=0)shot_frames=300;
    int ui_on = !shot || getenv("FXLAB_UI")!=NULL;   // UI off negli screenshot, FXLAB_UI la forza
    // verifica headless degli FX: FXLAB_FX=N -> al frame N esplode l'agente (gib
    // grande + cadavere + decal), così uno screenshot successivo mostra i pool.
    int fx_frame = getenv("FXLAB_FX") ? atoi(getenv("FXLAB_FX")) : -1; int fx_done=0;
    int fx_clip = getenv("FXLAB_CLIP") ? atoi(getenv("FXLAB_CLIP")) : -2;  // -2=off: forza una clip (verifica headless override)
    int fx_hit = getenv("FXLAB_HIT") ? atoi(getenv("FXLAB_HIT")) : -1; int fx_hit_done=0; // trigga un hit (verifica crossfade)
    int fx_body = getenv("FXLAB_BODY") ? atoi(getenv("FXLAB_BODY")) : -1;  // pin variante (verifica headless)
    int fx_out  = getenv("FXLAB_OUTFIT") ? atoi(getenv("FXLAB_OUTFIT")) : -1; // forza outfit (verifica headless)

    int drag_cam=0; float drag_anx=0,drag_any=0, mouse_px=0,mouse_py=0, rot_px=0,rot_py=0;
    const float FIXED_DT=1.0f/60.0f;
    mat4 vp; m_identity(vp);
    Uint64 pf=SDL_GetPerformanceFrequency(); Uint64 prev=SDL_GetPerformanceCounter(); double acc_t=0.0;
    int running=1,frame=0,shot_done=0;

    while(running){
        // l'UI ha la precedenza sul mouse: niente camera quando ci sopra/attiva.
        int ui_hot = ui_on && (nk_window_is_any_hovered(&nk.ctx) || nk_item_is_any_active(&nk.ctx));
        SDL_Event e; if(ui_on) nk_input_begin(&nk.ctx);
        while(SDL_PollEvent(&e)){
            if(ui_on) nkgl_event(&nk,&e);
            int liveIdx = simp_index_of(s,ah);   // -1 se morto
            int liveSlot = liveIdx>=0 ? simp_slot_of(s,liveIdx) : -1;
            float lx = liveIdx>=0 ? simp_px(s)[liveIdx] : Wm*0.5f;
            float ly = liveIdx>=0 ? simp_py(s)[liveIdx] : 1.2f;
            float lr = liveIdx>=0 ? simp_radius_arr(s)[liveIdx] : 0.30f;
            if(e.type==SDL_EVENT_QUIT) running=0;
            else if(e.type==SDL_EVENT_KEY_DOWN){
                SDL_Keycode k=e.key.key;
                if(k==SDLK_ESCAPE) running=0;
                else if(k==SDLK_SPACE) paused=!paused;
                else if(k==SDLK_C) cam_free=!cam_free;
                else if(k==SDLK_T) useTex=!useTex;
                else if(k==SDLK_P){ normal_lit=!normal_lit; printf("[fxlab] sagoma-cadavere: %s\n", normal_lit?"NORMAL (relight per-pixel)":"FLAT (bake congelato)"); }
                else if(k==SDLK_LEFTBRACKET && liveSlot>=0){ cur_var=(cur_var+NVAR-1)%NVAR; vat_layer_set_variant(vl,liveSlot,cur_var); }
                else if(k==SDLK_RIGHTBRACKET && liveSlot>=0){ cur_var=(cur_var+1)%NVAR; vat_layer_set_variant(vl,liveSlot,cur_var); }
                else if(k==SDLK_O && liveSlot>=0) vat_layer_make_bloody(vl,liveSlot);
                else if(k==SDLK_H && liveIdx>=0){ float d=vat_layer_hit(vl,liveSlot); if(d>0) simp_stun(s,liveIdx,d);
                    float o[3]={lx, ter_z(lx,ly)+1.0f, ly}; fx_emit(&fx,o,&BLOOD_DEF,0.0f,-1.0f);
                    ui_bloody=1; vat_layer_make_bloody(vl,liveSlot); }
                else if(k==SDLK_M && liveSlot>=0){ cur_var=CRAWLER_VAR; vat_layer_set_variant(vl,liveSlot,CRAWLER_VAR); }
                else if(k==SDLK_N && liveSlot>=0){
                    float hd=atan2f(simp_vy(s)[liveIdx],simp_vx(s)[liveIdx]);
                    vat_layer_maim_arm(vl,liveSlot,lx,ly,lr,hd,(unsigned)frame*2654435761u);
                    float o[3]={lx, ter_z(lx,ly)+1.2f, ly}; fx_emit(&fx,o,&BLOOD_DEF,0.0f,-1.0f);
                    cur_var=ARM_VAR; vat_layer_set_variant(vl,liveSlot,ARM_VAR);
                    ui_bloody=1; vat_layer_make_bloody(vl,liveSlot); }
                else if(k==SDLK_G && liveSlot>=0) vat_layer_gib_wound(vl,liveSlot,lx,ly,lr,(unsigned)frame*2654435761u);
                else if(k==SDLK_J && liveIdx>=0){ float o[3]={lx, ter_z(lx,ly)+1.0f, ly}; fx_emit(&fx,o,&BLOOD_DEF,0.0f,-1.0f); }
                else if((k==SDLK_K||k==SDLK_B) && liveIdx>=0){
                    if(k==SDLK_B){ vat_layer_explode(vl,liveSlot,lx,ly,lr,1.0f,(unsigned)frame*2654435761u);
                        float o[3]={lx, ter_z(lx,ly)+1.0f, ly}; fx_emit(&fx,o,&BLOOD_BURST_DEF,0.0f,-1.0f); }
                    else vat_layer_die(vl,liveSlot,lx,ly,lr);
                    simp_kill(s,liveIdx); respawn_t=1.2f;
                }
                else if(k==SDLK_R && liveIdx>=0){ simp_kill(s,liveIdx); respawn_t=0.05f; }
                else if(k==SDLK_LEFT) az-=0.05f; else if(k==SDLK_RIGHT) az+=0.05f;
                else if(k==SDLK_UP) el+=0.03f; else if(k==SDLK_DOWN) el-=0.03f;
                else if(k==SDLK_EQUALS||k==SDLK_KP_PLUS) hh*=0.9f;
                else if(k==SDLK_MINUS||k==SDLK_KP_MINUS) hh*=1.1f;
            }
            else if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN && !ui_hot){
                mouse_px=e.button.x; mouse_py=e.button.y;
                if(e.button.button==SDL_BUTTON_LEFT){ drag_cam=1; pick_y0(vp,mouse_px,mouse_py,SW,SH,&drag_anx,&drag_any); }
                else if(e.button.button==SDL_BUTTON_RIGHT){ drag_cam=2; rot_px=mouse_px; rot_py=mouse_py; }
            }
            else if(e.type==SDL_EVENT_MOUSE_BUTTON_UP) drag_cam=0;
            else if(e.type==SDL_EVENT_MOUSE_MOTION){ mouse_px=e.motion.x; mouse_py=e.motion.y; }
            else if(e.type==SDL_EVENT_MOUSE_WHEEL && !ui_hot){ hh*= (e.wheel.y>0)?0.9f:1.1f; if(hh<2)hh=2; if(hh>200)hh=200; }
        }
        if(ui_on) nk_input_end(&nk.ctx);
        if(el<0.08f)el=0.08f; if(el>1.50f)el=1.50f;

        // --- driver scriptato + sim
        if(!paused){
            acc_t += (double)(SDL_GetPerformanceCounter()-prev)/pf;
        }
        prev=SDL_GetPerformanceCounter();
        if(shot) acc_t = FIXED_DT;   // headless: un passo fisso per frame
        if(paused) acc_t = 0;
        int steps=0;
        while(acc_t>=FIXED_DT && steps<8){
            // respawn dell'agente se morto
            if(simp_index_of(s,ah)<0){
                respawn_t-=FIXED_DT;
                if(respawn_t<=0){ toward_top=1; set_goal_band(s,gw,gh,toward_top);
                    int ni=simp_spawn(s, Wm*0.5f, 1.2f);
                    if(ni>=0){ ah=simp_handle_of(s,ni); int sl=simp_slot_of(s,ni);
                        vat_layer_pin_variant(vl,sl,cur_var);                 // mantiene body...
                        vat_layer_set_outfit(vl,sl, ui_outfit+(ui_bloody?16:0)); } }  // ...e outfit scelti
            } else {
                // spola: alla fine del percorso ribalta il goal
                int li=simp_index_of(s,ah); float py=simp_py(s)[li];
                if(toward_top && py>Hm-1.6f){ toward_top=0; set_goal_band(s,gw,gh,toward_top); }
                else if(!toward_top && py<1.6f){ toward_top=1; set_goal_band(s,gw,gh,toward_top); }
            }
            // trigger FX headless (morte esplosiva dell'agente)
            if(fx_frame>=0 && !fx_done && frame>=fx_frame){ int li=simp_index_of(s,ah);
                if(li>=0){ int sl=simp_slot_of(s,li); float gx=simp_px(s)[li],gy=simp_py(s)[li],gr=simp_radius_arr(s)[li];
                    vat_layer_explode(vl,sl,gx,gy,gr,1.0f,12345u);
                    float o[3]={gx, ter_z(gx,gy)+1.0f, gy}; fx_emit(&fx,o,&BLOOD_BURST_DEF,0.0f,-1.0f);
                    simp_kill(s,li); respawn_t=1e9f; }
                fx_done=1; }
            if(fx_clip!=-2){ int li=simp_index_of(s,ah); if(li>=0) vat_layer_force_clip(vl,simp_slot_of(s,li),fx_clip); }
            if(fx_body>=0){ int li=simp_index_of(s,ah); if(li>=0) vat_layer_set_variant(vl,simp_slot_of(s,li),fx_body); }
            if(fx_out>=0){ int li=simp_index_of(s,ah); if(li>=0) vat_layer_set_outfit(vl,simp_slot_of(s,li),fx_out); }
            if(fx_hit>=0 && !fx_hit_done && frame>=fx_hit){ int li=simp_index_of(s,ah);
                if(li>=0){ float d=vat_layer_hit(vl,simp_slot_of(s,li)); if(d>0)simp_stun(s,li,d); } fx_hit_done=1; }
            simp_step(s,FIXED_DT);
            vat_layer_update(vl,s,FIXED_DT);
            fx_update(&fx,FIXED_DT,NULL,fx_ground,NULL);
            acc_t-=FIXED_DT; steps++; frame++;
        }

        // --- pannello UI (nuklear): body/outfit/effetti sull'agente vivo
        if(ui_on){
            int li=simp_index_of(s,ah), sl=li>=0?simp_slot_of(s,li):-1;
            float ex=li>=0?simp_px(s)[li]:Wm*0.5f, ey=li>=0?simp_py(s)[li]:1.2f, er=li>=0?simp_radius_arr(s)[li]:0.30f;
            unsigned seed=(unsigned)frame*2654435761u;
            struct nk_context *c=&nk.ctx;
            if(nk_begin(c,"FX Lab",nk_rect(8,8,236,422),
                        NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_TITLE|NK_WINDOW_MINIMIZABLE)){
                nk_layout_row_dynamic(c,18,1);
                nk_label(c, li>=0?"agente: VIVO":"agente: morto", NK_TEXT_LEFT);
                nk_layout_row_dynamic(c,20,1); nk_label(c,"Body",NK_TEXT_LEFT);
                int nb=nk_combo(c,BODY_NAME,NVAR,cur_var,20,nk_vec2(210,240));
                if(nb!=cur_var){ cur_var=nb; if(sl>=0) vat_layer_set_variant(vl,sl,cur_var); }
                nk_layout_row_dynamic(c,20,1); nk_label(c,"Outfit (0-13, 14=carbon 15=acido)",NK_TEXT_LEFT);
                int no=nk_propertyi(c,"base",0,ui_outfit,15,1,0.2f);
                int nbl=nk_check_label(c,"insanguinato",ui_bloody)?1:0;
                if(no!=ui_outfit||nbl!=ui_bloody){ ui_outfit=no; ui_bloody=nbl;
                    if(sl>=0) vat_layer_set_outfit(vl,sl,ui_outfit+(ui_bloody?16:0)); }

                // Animazione di locomozione: elenca le clip walk/run/idle del body
                // corrente; "auto" = FSM velocità→stato. Cambiare body resetta ad auto.
                if(anim_body!=cur_var){ anim_body=cur_var; ui_force_clip=-1; if(sl>=0) vat_layer_force_clip(vl,sl,-1); }
                const VatMeta *Mb=vat_layer_meta_variant(vl,cur_var);
                static char albl[24][40]; const char *aptr[24]; int aidx[24]; int an=0;
                snprintf(albl[0],40,"auto (FSM)"); aptr[0]=albl[0]; aidx[0]=-1; an=1;
                for(int ci=0; ci<Mb->nclips && an<24; ci++){ const char *nm=Mb->clip[ci].name;
                    if(!strncmp(nm,"walk",4)||!strncmp(nm,"run",3)||!strncmp(nm,"idle",4)){
                        snprintf(albl[an],40,"%s",nm); aptr[an]=albl[an]; aidx[an]=ci; an++; } }
                int asel=0; for(int j=0;j<an;j++) if(aidx[j]==ui_force_clip) asel=j;
                nk_layout_row_dynamic(c,20,1); nk_label(c,"Animazione",NK_TEXT_LEFT);
                int anew=nk_combo(c,aptr,an,asel,20,nk_vec2(210,260));
                if(anew!=asel){ ui_force_clip=aidx[anew]; if(sl>=0) vat_layer_force_clip(vl,sl,ui_force_clip); }
                // Pannello AUTOREVOLE: applica body+outfit+clip scelti all'agente vivo
                // OGNI frame, così lo schermo combacia sempre col pannello (anche al
                // primo spawn e dopo i respawn) — niente più body casuale fuori sync.
                if(sl>=0){ vat_layer_set_variant(vl,sl,cur_var);
                    vat_layer_set_outfit(vl,sl,ui_outfit+(ui_bloody?16:0));
                    if(ui_force_clip>=0) vat_layer_force_clip(vl,sl,ui_force_clip); }

                nk_layout_row_dynamic(c,22,2);
                if(nk_button_label(c,"Hit+sangue")&&li>=0){ float d=vat_layer_hit(vl,sl); if(d>0)simp_stun(s,li,d);
                    float o[3]={ex, ter_z(ex,ey)+1.0f, ey}; fx_emit(&fx,o,&BLOOD_DEF,0.0f,-1.0f);
                    ui_bloody=1; vat_layer_make_bloody(vl,sl); }
                if(nk_button_label(c,"Maim")&&sl>=0){ cur_var=CRAWLER_VAR; vat_layer_set_variant(vl,sl,CRAWLER_VAR); }
                if(nk_button_label(c,"Maim braccio")&&li>=0){
                    float hd=atan2f(simp_vy(s)[li],simp_vx(s)[li]);
                    vat_layer_maim_arm(vl,sl,ex,ey,er,hd,seed);
                    float o[3]={ex, ter_z(ex,ey)+1.2f, ey}; fx_emit(&fx,o,&BLOOD_DEF,0.0f,-1.0f);
                    cur_var=ARM_VAR; vat_layer_set_variant(vl,sl,ARM_VAR);
                    ui_bloody=1; vat_layer_make_bloody(vl,sl); }
                if(nk_button_label(c,"Maim gambe")&&li>=0){
                    float hd=atan2f(simp_vy(s)[li],simp_vx(s)[li]);
                    vat_layer_maim_legs(vl,sl,ex,ey,er,hd,seed);
                    float o[3]={ex, ter_z(ex,ey)+0.8f, ey}; fx_emit(&fx,o,&BLOOD_DEF,0.0f,-1.0f);
                    cur_var=CRAWLER_VAR; vat_layer_set_variant(vl,sl,CRAWLER_VAR);
                    ui_bloody=1; vat_layer_make_bloody(vl,sl); }
                if(nk_button_label(c,"Schizzo")&&sl>=0) vat_layer_gib_wound(vl,sl,ex,ey,er,seed);
                if(nk_button_label(c,"Sangue")&&li>=0){ float o[3]={ex, ter_z(ex,ey)+1.0f, ey}; fx_emit(&fx,o,&BLOOD_DEF,0.0f,-1.0f); }
                if(nk_button_label(c,"Insanguina")&&sl>=0){ ui_bloody=1; vat_layer_make_bloody(vl,sl); }
                if(nk_button_label(c,"Morte")&&li>=0){ vat_layer_die(vl,sl,ex,ey,er); simp_kill(s,li); respawn_t=1.2f; }
                if(nk_button_label(c,"Esplodi")&&li>=0){ vat_layer_explode(vl,sl,ex,ey,er,1.0f,seed);
                    float o[3]={ex, ter_z(ex,ey)+1.0f, ey}; fx_emit(&fx,o,&BLOOD_BURST_DEF,0.0f,-1.0f);
                    simp_kill(s,li); respawn_t=1.2f; }
                if(nk_button_label(c,"Smembra")&&li>=0){ vat_layer_explode(vl,sl,ex,ey,er,0.4f,seed);
                    float o[3]={ex, ter_z(ex,ey)+1.0f, ey}; fx_emit(&fx,o,&BLOOD_BURST_DEF,0.0f,-1.0f);
                    simp_kill(s,li); respawn_t=1.2f; }
                nk_layout_row_dynamic(c,22,2);
                if(nk_button_label(c,"Respawn")&&li>=0){ simp_kill(s,li); respawn_t=0.05f; }
                paused = nk_check_label(c,"pausa",paused)?1:0;
                nk_layout_row_dynamic(c,18,1);
                nk_labelf(c,NK_TEXT_LEFT,"body:%s  outfit:%d",BODY_NAME[cur_var],ui_outfit+(ui_bloody?16:0));
            }
            nk_end(c);
        }

        // --- render
        if(!shot) SDL_GetWindowSizeInPixels(win,&SW,&SH);
        if(drag_cam==1){ float xw,yw; if(pick_y0(vp,mouse_px,mouse_py,SW,SH,&xw,&yw)){ cx+=drag_anx-xw; cz+=drag_any-yw; } }
        else if(drag_cam==2){ az-=(mouse_px-rot_px)*0.005f; el+=(mouse_py-rot_py)*0.005f;
            if(el<0.08f)el=0.08f; if(el>1.50f)el=1.50f; rot_px=mouse_px; rot_py=mouse_py; }
        float asp=(float)SW/SH; mat4 proj,view; float ctr[3]={cx,0.9f,cz},up[3]={0,1,0};
        float eye[3]={cx+hh*cosf(el)*sinf(az),0.9f+hh*sinf(el),cz+hh*cosf(el)*cosf(az)};
        if(cam_free)m_persp(proj,45.0f*3.14159f/180.0f,asp,0.1f,500.0f);
        else m_ortho(proj,-hh*asp,hh*asp,-hh,hh,-200,400);
        m_lookat(view,eye,ctr,up); m_mul(vp,proj,view);

        glViewport(0,0,SW,SH);glClearColor(0.12f,0.13f,0.16f,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

        if(groundOn){ glUseProgram(progGnd);glUniformMatrix4fv(uVPgnd,1,GL_FALSE,vp);
            glUniform1i(uHasGnd, useTex && gnd.hasTex); glUniform3f(uColGnd,0.30f,0.32f,0.26f);
            glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,gnd.tex);
            glBindVertexArray(gnd.vao);glDrawElements(GL_TRIANGLES,gnd.nidx,GL_UNSIGNED_INT,0); }

        // gib (box balistici)
        { int ng=vat_layer_fill_gibs(vl,gibparam,GIBMAX); int gc=0;
          for(int gi=0;gi<ng;gi++){ float *p=gibparam+gi*10; float zb=ter_z(p[0],p[1])+p[2];
              gc=prop_box(gibmesh,gc, p[0],p[1], 0.0f,0.0f, zb, p[3],p[5],p[4], cosf(p[6]),sinf(p[6]), p[7],p[8],p[9]); }
          if(gc){ glUseProgram(progFlat);glUniformMatrix4fv(uVPflat,1,GL_FALSE,vp);
              glBindVertexArray(gibVao);glBindBuffer(GL_ARRAY_BUFFER,gibVbo);
              glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)gc*9*sizeof(float),gibmesh);
              glDrawArrays(GL_TRIANGLES,0,gc); } }

        // gore mesh-gibs (arm + frammenti): mesh statiche texturizzate, tumble 3D
        if(nGibMesh>0){ int nm=vat_layer_fill_mesh_gibs(vl,meshgib,256);
          if(nm){ glUseProgram(mProg);glUniformMatrix4fv(uVPm,1,GL_FALSE,vp);
              glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,A[0].texD);
              glUniform1i(glGetUniformLocation(mProg,"uTex"),0);
              for(int i=0;i<nm;i++){ float *p=meshgib+i*9; int mid=(int)(p[0]+0.5f);
                  if(mid<0||mid>=nGibMesh) continue;
                  float wx=p[1], wz=p[2], wy=ter_z(p[1],p[2])+p[3];
                  mat4 mm; gib_model(mm, wx,wy,wz, p[4],p[5],p[6], p[7], p[8]*GIB_UNIT);
                  glUniformMatrix4fv(uModelm,1,GL_FALSE,mm);
                  glBindVertexArray(GM[mid].vao);
                  glDrawElements(GL_TRIANGLES,GM[mid].nidx,GL_UNSIGNED_SHORT,0); } } }

        // decal di sangue persistenti (a terra, blended)
        { int nd=vat_layer_fill_decals(vl,decalraw,DECALMAX);
          for(int i=0;i<nd;i++){ float *r=decalraw+i*6,*o=decalinst+i*7;
              o[0]=r[0]; o[1]=ter_z(r[0],r[1])+0.03f; o[2]=r[1]; o[3]=r[2]; o[4]=r[3]; o[5]=r[4]; o[6]=r[5]; }
          if(nd){ glUseProgram(progDc);glUniformMatrix4fv(uVPdc,1,GL_FALSE,vp);
              glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDepthMask(GL_FALSE);
              glBindVertexArray(dcVao);glBindBuffer(GL_ARRAY_BUFFER,dcInst);
              glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)nd*7*sizeof(float),decalinst);
              glDrawArraysInstanced(GL_TRIANGLE_FAN,0,SHADN,nd);
              glDepthMask(GL_TRUE);glDisable(GL_BLEND); } }

        // sagome-cadavere persistenti (quad orientati)
        { int nq=vat_layer_fill_corpse_decals(vl,cdecraw,CORPSEDECMAX);
          for(int i=0;i<nq;i++){ float *r=cdecraw+i*6,*o=cdecinst+i*7;
              o[0]=r[0]; o[1]=ter_z(r[0],r[1])+0.05f; o[2]=r[1]; o[3]=r[2];   // pos+heading
              o[4]=CORPSE_HALF*r[3]; o[5]=r[4]; o[6]=r[5]; }                   // half, colonna, outfit
          if(nq){ glUseProgram(cdProg);glUniformMatrix4fv(uVPcd,1,GL_FALSE,vp);
              glUniform1f(uNColsCd,(float)corpseNCols); glUniform1f(uNOutCd,(float)corpseNOut);
              glUniform1i(uNormLitCd,normal_lit);
              glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,corpseAlb);
              glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,corpseNrm);
              glUniform1i(glGetUniformLocation(cdProg,"uAlbedo"),0);
              glUniform1i(glGetUniformLocation(cdProg,"uNormal"),1);
              glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDepthMask(GL_FALSE);
              glBindVertexArray(cdVao);glBindBuffer(GL_ARRAY_BUFFER,cdInst);
              glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)nq*7*sizeof(float),cdecinst);
              glDrawArraysInstanced(GL_TRIANGLES,0,6,nq);
              glDepthMask(GL_TRUE);glDisable(GL_BLEND); } }

        // ombre a terra (sotto l'agente)
        { const float *px=simp_px(s),*py=simp_py(s),*rad=simp_radius_arr(s),*za=simp_z_arr(s);
          int n=simp_count(s), nsh=0;
          for(int i=0;i<n && nsh<MAXSLOT;i++){ float ax=px[i],ay=py[i],fz=za?za[i]:0.0f; float *o=inst+nsh*4;
              o[0]=ax; o[1]=ter_z(ax,ay)+0.02f; o[2]=ay; o[3]=rad[i]*1.35f/(1.0f+0.20f*fz); nsh++; }
          if(nsh){ glUseProgram(progSh);glUniformMatrix4fv(uVPsh,1,GL_FALSE,vp);
              glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDepthMask(GL_FALSE);
              glBindVertexArray(shVao);glBindBuffer(GL_ARRAY_BUFFER,shInst);
              glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)nsh*4*sizeof(float),inst);
              glDrawArraysInstanced(GL_TRIANGLE_FAN,0,SHADN,nsh);
              glDepthMask(GL_TRUE);glDisable(GL_BLEND); } }

        // agente VAT (per variante)
        glUseProgram(prog);glUniformMatrix4fv(uVP,1,GL_FALSE,vp);
        glUniform1i(glGetUniformLocation(prog,"texPos"),0);
        glUniform1i(glGetUniformLocation(prog,"texNorm"),1);
        glUniform1i(glGetUniformLocation(prog,"texDiff"),2);
        for(int v=0;v<NVAR;v++){
            int count=vat_layer_fill_variant(vl,s,v,inst,MAXSLOT);
            if(!count) continue;
            for(int q=0;q<count;q++) inst[q*14+1]+=ter_z(inst[q*14+0],inst[q*14+2]);
            const VatMeta *M=A[v].M;
            glBindBuffer(GL_ARRAY_BUFFER,bi);glBufferSubData(GL_ARRAY_BUFFER,0,count*14*sizeof(float),inst);
            glUniform2f(uTS,(float)M->texW,(float)M->texH);glUniform1f(uRPF,(float)M->rowsPerFrame);
            glUniform1i(uHas,useTex&&A[v].hasTex);
            glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,A[v].texP);
            glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,A[v].texN);
            glActiveTexture(GL_TEXTURE2);glBindTexture(GL_TEXTURE_2D,A[v].texD);
            glBindVertexArray(A[v].vao);glDrawElementsInstanced(GL_TRIANGLES,A[v].ni,GL_UNSIGNED_SHORT,0,count);
        }
        glBindVertexArray(0);

        // particle system: billboard istanziati, due passate (alpha poi additivo)
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

        if(ui_on) nkgl_render(&nk,SW,SH);   // UI sopra la scena
        SDL_GL_SwapWindow(win);

        if(shot && !shot_done && frame>=shot_frames){
            unsigned char *rgb=malloc((size_t)SW*SH*3);
            glReadPixels(0,0,SW,SH,GL_RGB,GL_UNSIGNED_BYTE,rgb);
            vg_save_bmp("fxlab_shot.bmp",SW,SH,rgb); free(rgb);
            printf("fxlab_shot.bmp (%dx%d) @ frame %d\n",SW,SH,frame);
            shot_done=1; running=0;
        }
    }
    free(gibmesh);
    vat_layer_destroy(vl); simp_destroy(s); if(gTerOn) terrain_free(&gTer);
    SDL_GL_DestroyContext(ctx); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
