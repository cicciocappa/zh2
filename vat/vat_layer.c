#include "vat_layer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

enum { ST_IDLE, ST_WALK, ST_RUN, ST_ATTACK, ST_SCREAM, ST_N };
static const char *st_prefix[ST_N] = {"idle","walk","run","attack","scream"};

#define HEADING_TAU   0.25f
#define SPEED_TAU     0.20f
#define BLEND_DUR     0.25f
#define MOVE_MIN      0.30f   /* sotto = heading congelato (anti-piroetta)   */
#define HEADING_OFFSET 0.0f  /* allinea il forward del modello (Mixamo +Y) a +v */

struct VatLayer {
    VatMeta m[VAT_MAX_VARIANTS]; int nvar;   /* un asset VAT per body type */
    int group_idx[ST_N][16], group_n[ST_N];  /* clip layout identico fra varianti */
    int max;
    SimPHandle *seen;
    float *hx, *hy, *spd, *phaseA, *phaseB, *blendF, *hmul;
    int   *clipA, *clipB;
    unsigned char *state, *target, *blending, *outfit, *var, *tr, *tg, *tb;
};

static unsigned hashu(unsigned a){ a^=a>>16; a*=2654435761u; a^=a>>13; a*=2246822519u; a^=a>>16; return a; }

static void load_meta(VatMeta *m, const char *path){
    FILE *f=fopen(path,"rb"); if(!f){fprintf(stderr,"vat_layer: no meta %s\n",path);return;}
    char line[256];
    while(fgets(line,sizeof line,f)){
        if(!strncmp(line,"clip=",5) && m->nclips<VAT_MAX_CLIPS){ VatClip *c=&m->clip[m->nclips];
            if(sscanf(line,"clip=%31s startFrame=%d numFrames=%d duration_s=%f stride_m=%f",
                      c->name,&c->startFrame,&c->numFrames,&c->duration,&c->stride)>=3) m->nclips++;
        } else { char *eq=strchr(line,'='); if(eq){*eq=0; char*k=line,*v=eq+1;
            if(!strcmp(k,"texW"))m->texW=atoi(v); else if(!strcmp(k,"texH"))m->texH=atoi(v);
            else if(!strcmp(k,"rowsPerFrame"))m->rowsPerFrame=atoi(v); else if(!strcmp(k,"fps"))m->fps=atof(v);
            else if(!strcmp(k,"scale"))m->scale=atof(v); else if(!strcmp(k,"totalFrames"))m->total=atoi(v);} }
    }
    fclose(f);
}

VatLayer *vat_layer_create_multi(const char *const *meta_paths, int nvariants, int max_slots){
    VatLayer *vl=calloc(1,sizeof *vl); vl->max=max_slots;
    if(nvariants<1)nvariants=1; if(nvariants>VAT_MAX_VARIANTS)nvariants=VAT_MAX_VARIANTS;
    vl->nvar=nvariants;
    for(int v=0;v<nvariants;v++) load_meta(&vl->m[v], meta_paths[v]);
    /* I gruppi FSM (quali clip sono idle/walk/run) vengono dalla variante 0:
       il layout di clip è identico fra tutti gli asset (stesso ORDER di bake). */
    for(int s=0;s<ST_N;s++)vl->group_n[s]=0;
    for(int i=0;i<vl->m[0].nclips;i++) for(int s=0;s<ST_N;s++)
        if(!strncmp(vl->m[0].clip[i].name,st_prefix[s],strlen(st_prefix[s]))){
            if(vl->group_n[s]<16)vl->group_idx[s][vl->group_n[s]++]=i;
            break; }
    int n=max_slots;
    vl->seen=calloc(n,sizeof(SimPHandle));
    vl->hx=calloc(n,4); vl->hy=calloc(n,4); vl->spd=calloc(n,4);
    vl->phaseA=calloc(n,4); vl->phaseB=calloc(n,4); vl->blendF=calloc(n,4); vl->hmul=calloc(n,4);
    vl->clipA=calloc(n,sizeof(int)); vl->clipB=calloc(n,sizeof(int));
    vl->state=calloc(n,1); vl->target=calloc(n,1); vl->blending=calloc(n,1);
    vl->outfit=calloc(n,1); vl->var=calloc(n,1); vl->tr=calloc(n,1); vl->tg=calloc(n,1); vl->tb=calloc(n,1);
    return vl;
}
VatLayer *vat_layer_create(const char *meta_path, int max_slots){
    return vat_layer_create_multi(&meta_path, 1, max_slots);
}
void vat_layer_destroy(VatLayer *vl){ if(!vl)return;
    free(vl->seen);free(vl->hx);free(vl->hy);free(vl->spd);free(vl->phaseA);free(vl->phaseB);
    free(vl->blendF);free(vl->hmul);free(vl->clipA);free(vl->clipB);free(vl->state);free(vl->target);
    free(vl->blending);free(vl->outfit);free(vl->var);free(vl->tr);free(vl->tg);free(vl->tb);free(vl); }
int vat_layer_nvariants(const VatLayer *vl){ return vl->nvar; }
const VatMeta *vat_layer_meta_variant(const VatLayer *vl, int variant){
    if(variant<0||variant>=vl->nvar)variant=0; return &vl->m[variant]; }
const VatMeta *vat_layer_meta(const VatLayer *vl){ return &vl->m[0]; }

static int pick_variant(VatLayer *vl,int slot,int st){
    int n=vl->group_n[st]; if(n<=0) return 0;
    return vl->group_idx[st][ hashu(slot*131u+st*977u) % (unsigned)n ]; }

/* stato voluto dalla velocità, con isteresi */
static int want_state(int cur,float spd){
    if(cur==ST_IDLE) return spd>0.40f ? ST_WALK : ST_IDLE;
    if(cur==ST_RUN)  return spd<2.00f ? ST_WALK : ST_RUN;
    if(spd<0.20f) return ST_IDLE;
    if(spd>2.40f) return ST_RUN;
    return ST_WALK; /* da WALK */
}

void vat_layer_update(VatLayer *vl, const SimP *s, float dt){
    const float *vx=simp_vx(s),*vy=simp_vy(s); const unsigned char *fl=simp_flags_arr(s);
    int n=simp_count(s);
    float alpha=1.0f-expf(-dt/HEADING_TAU), beta=1.0f-expf(-dt/SPEED_TAU);
    for(int i=0;i<n;i++){
        int slot=simp_slot_of(s,i); if(slot<0||slot>=vl->max) continue;
        SimPHandle h=simp_handle_of(s,i);
        if(vl->seen[slot]!=h){                       /* nuovo agente nello slot */
            unsigned r=hashu(h); float ang=(r&0xffff)*(6.2831853f/65536.0f);
            vl->seen[slot]=h; vl->hx[slot]=sinf(ang)*0.01f; vl->hy[slot]=cosf(ang)*0.01f;
            vl->spd[slot]=1.0f; vl->state[slot]=ST_WALK; vl->blending[slot]=0;
            vl->clipA[slot]=pick_variant(vl,slot,ST_WALK); vl->phaseA[slot]=(float)((r>>16)&0xff)/256.0f;
            vl->var[slot]=(unsigned char)(hashu(h+333u)%(unsigned)vl->nvar); /* body model, cosmetico */
            vl->outfit[slot]=(unsigned char)(hashu(h+777u)%16u);
            vl->hmul[slot]=0.90f+(hashu(h+555u)%1000)/1000.0f*0.22f; /* altezza ±, cosmetico */
            static const unsigned char PAL[8][3]={{255,255,255},{215,255,215},{255,225,210},{225,225,240},
                {255,245,205},{235,215,215},{225,240,225},{240,240,215}};
            const unsigned char *p=PAL[(r>>8)&7]; int j=(int)((r>>11)&0x1f)-16;
            int tr=p[0]+j,tg=p[1]+j,tb=p[2]+j;
            vl->tr[slot]=tr>255?255:(tr<160?160:tr); vl->tg[slot]=tg>255?255:(tg<160?160:tg); vl->tb[slot]=tb>255?255:(tb<160?160:tb);
        }
        float sp=sqrtf(vx[i]*vx[i]+vy[i]*vy[i]);
        vl->spd[slot]+=beta*(sp-vl->spd[slot]);

        /* heading EMA, congelato se quasi fermo (anti-piroetta) o in volo */
        if(sp>MOVE_MIN && !(fl[i]&SIMP_FLYING)){
            vl->hx[slot]+=alpha*(vx[i]-vl->hx[slot]); vl->hy[slot]+=alpha*(vy[i]-vl->hy[slot]); }

        /* FSM da velocità (dormienti = idle) */
        int cur=vl->state[slot];
        int want=(fl[i]&SIMP_DORMANT)?ST_IDLE:want_state(cur,vl->spd[slot]);
        int eff=vl->blending[slot]?vl->target[slot]:cur;
        if(want!=eff && vl->group_n[want]>0){
            vl->blending[slot]=1; vl->target[slot]=want;
            vl->clipB[slot]=pick_variant(vl,slot,want); vl->phaseB[slot]=0; vl->blendF[slot]=0; }

        /* avanza fase (distanza per locomozione, tempo per il resto) — stride/durata
           dal meta del BODY di questo agente (variano per taglia: il bambino ha
           stride diverso dall'adulto). */
        const VatMeta *M=&vl->m[vl->var[slot]];
        float dist=sp*dt;
        const VatClip *ca=&M->clip[vl->clipA[slot]];
        vl->phaseA[slot]+= ca->stride>0.1f ? dist/ca->stride : dt/(ca->duration>0?ca->duration:1.0f);
        vl->phaseA[slot]-=floorf(vl->phaseA[slot]);
        if(vl->blending[slot]){
            const VatClip *cb=&M->clip[vl->clipB[slot]];
            vl->phaseB[slot]+= cb->stride>0.1f ? dist/cb->stride : dt/(cb->duration>0?cb->duration:1.0f);
            vl->phaseB[slot]-=floorf(vl->phaseB[slot]);
            vl->blendF[slot]+=dt/BLEND_DUR;
            if(vl->blendF[slot]>=1.0f){ vl->state[slot]=vl->target[slot];
                vl->clipA[slot]=vl->clipB[slot]; vl->phaseA[slot]=vl->phaseB[slot]; vl->blending[slot]=0; }
        }
    }
}

/* Emette una istanza (12 float) usando il meta `M` passato (= body dell'agente).
   I frame gA/gB sono indici nella VAT texture di QUELLA variante. */
static void emit_instance(VatLayer *vl, int slot, const VatMeta *M,
                          float x, float y, float z, float r, float *o){
    const VatClip *ca=&M->clip[vl->clipA[slot]];
    float gA,gB,mix;
    if(vl->blending[slot]){ const VatClip *cb=&M->clip[vl->clipB[slot]];
        float la=floorf(vl->phaseA[slot]*ca->numFrames), lb=floorf(vl->phaseB[slot]*cb->numFrames);
        gA=ca->startFrame+fmodf(la,(float)ca->numFrames); gB=cb->startFrame+fmodf(lb,(float)cb->numFrames);
        mix=vl->blendF[slot]>1?1:vl->blendF[slot];
    } else { float local=vl->phaseA[slot]*ca->numFrames; float fa=floorf(local),fb=fa+1; if(fb>=ca->numFrames)fb=0;
        gA=ca->startFrame+fa; gB=ca->startFrame+fb; mix=local-fa; }
    float head=atan2f(vl->hx[slot],vl->hy[slot])+HEADING_OFFSET;
    o[0]=x; o[1]=z; o[2]=y; o[3]=head;
    o[4]=(r/0.30f)*vl->hmul[slot]; o[5]=gA; o[6]=gB; o[7]=mix; o[8]=(float)vl->outfit[slot];
    o[9]=vl->tr[slot]/255.0f; o[10]=vl->tg[slot]/255.0f; o[11]=vl->tb[slot]/255.0f;
}

int vat_layer_fill_variant(VatLayer *vl, const SimP *s, int variant, float *buf, int max_inst){
    if(variant<0||variant>=vl->nvar) return 0;
    const VatMeta *M=&vl->m[variant];
    const float *px=simp_px(s),*py=simp_py(s),*rad=simp_radius_arr(s),*za=simp_z_arr(s);
    int n=simp_count(s), c=0;
    for(int i=0;i<n && c<max_inst;i++){
        int slot=simp_slot_of(s,i); if(slot<0||slot>=vl->max) continue;
        if(vl->var[slot]!=variant) continue;
        emit_instance(vl,slot,M,px[i],py[i],za?za[i]:0.0f,rad[i],buf+c*12);
        c++;
    }
    return c;
}

int vat_layer_fill(VatLayer *vl, const SimP *s, float *buf, int max_inst){
    const float *px=simp_px(s),*py=simp_py(s),*rad=simp_radius_arr(s),*za=simp_z_arr(s);
    int n=simp_count(s), c=0;
    for(int i=0;i<n && c<max_inst;i++){
        int slot=simp_slot_of(s,i); if(slot<0||slot>=vl->max) continue;
        emit_instance(vl,slot,&vl->m[vl->var[slot]],px[i],py[i],za?za[i]:0.0f,rad[i],buf+c*12);
        c++;
    }
    return c;
}
