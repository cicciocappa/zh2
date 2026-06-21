#include "vat_layer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

enum { ST_IDLE, ST_WALK, ST_RUN, ST_ATTACK, ST_SCREAM, ST_HIT, ST_DYING, ST_DEATH, ST_N };
static const char *st_prefix[ST_N] = {"idle","walk","run","attack","scream","hit","dying","death"};

#define HEADING_TAU   0.25f
#define SPEED_TAU     0.20f
#define BLEND_DUR     0.25f
#define MOVE_MIN      0.30f   /* sotto = heading congelato (anti-piroetta)   */
#define HEADING_OFFSET 0.0f  /* allinea il forward del modello (Mixamo +Y) a +v */
#define ATTACK_PRESS  0.006f  /* wall_pressure oltre cui l'agente "attacca": allineato
                                 ad ATTACK_MIN_P di defense.c (animazione ⇔ assedio reale) */
#define DECEDENT_HOLD 2.0f    /* s di tenuta dell'ultimo frame morte prima di liberare */

struct VatLayer {
    VatMeta m[VAT_MAX_VARIANTS]; int nvar;   /* un asset VAT per body type */
    /* gruppi FSM PER-VARIANTE: ogni body puo' avere un layout di clip diverso
       (es. il crawler ha 3 clip walk/run/idle a startFrame propri, gli zombie
       normali ne hanno 13). Indicizzati [variante][stato]. */
    int group_idx[VAT_MAX_VARIANTS][ST_N][16], group_n[VAT_MAX_VARIANTS][ST_N];
    int nrandom;                             /* varianti 0..nrandom-1 assegnabili a caso (cosmetiche);
                                                le altre solo via vat_layer_pin_variant (tipi di gioco) */
    int max;
    SimPHandle *seen;
    float *hx, *hy, *spd, *phaseA, *phaseB, *blendF, *hmul;
    int   *clipA, *clipB, *pin;              /* pin[slot] = variante+1 forzata, 0 = libera */
    unsigned char *state, *target, *blending, *outfit, *var, *tr, *tg, *tb;
    /* one-shot HIT overlay (interrompe la FSM per la durata della clip, poi torna) */
    float *osT, *osPh; int *osClip;          /* osT[slot]>0 = flinch in corso */
    /* pool decessi renderer-side: visuali che riproducono la clip morte una volta
       e poi tengono l'ultimo frame, indipendenti dall'agente sim gia' rimosso. */
    int dmax, dwrite;
    float *dx, *dy, *dz, *dhd, *dsc, *dph, *dttl;
    unsigned char *dvar, *dclip, *dout, *dtr, *dtg, *dtb, *dactive;
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
    /* Gruppi FSM PER-VARIANTE: per ogni body, quali clip sono idle/walk/run
       (match per prefisso del nome). Cosi' un body con layout diverso (il
       crawler: walk/run/idle invece dei 13 standard) funziona con la stessa
       FSM velocita'->stato. */
    for(int v=0;v<nvariants;v++){
        for(int s=0;s<ST_N;s++)vl->group_n[v][s]=0;
        for(int i=0;i<vl->m[v].nclips;i++) for(int s=0;s<ST_N;s++)
            if(!strncmp(vl->m[v].clip[i].name,st_prefix[s],strlen(st_prefix[s]))){
                if(vl->group_n[v][s]<16)vl->group_idx[v][s][vl->group_n[v][s]++]=i;
                break; }
    }
    vl->nrandom=nvariants;                    /* default: tutte cosmetiche */
    int n=max_slots;
    vl->seen=calloc(n,sizeof(SimPHandle));
    vl->hx=calloc(n,4); vl->hy=calloc(n,4); vl->spd=calloc(n,4);
    vl->phaseA=calloc(n,4); vl->phaseB=calloc(n,4); vl->blendF=calloc(n,4); vl->hmul=calloc(n,4);
    vl->clipA=calloc(n,sizeof(int)); vl->clipB=calloc(n,sizeof(int)); vl->pin=calloc(n,sizeof(int));
    vl->state=calloc(n,1); vl->target=calloc(n,1); vl->blending=calloc(n,1);
    vl->outfit=calloc(n,1); vl->var=calloc(n,1); vl->tr=calloc(n,1); vl->tg=calloc(n,1); vl->tb=calloc(n,1);
    vl->osT=calloc(n,4); vl->osPh=calloc(n,4); vl->osClip=calloc(n,sizeof(int));
    /* pool decessi: dimensionato a ~min(max,4096) come i cadaveri (M3.3) */
    vl->dmax = max_slots<4096?max_slots:4096; vl->dwrite=0;
    int d=vl->dmax;
    vl->dx=calloc(d,4); vl->dy=calloc(d,4); vl->dz=calloc(d,4); vl->dhd=calloc(d,4);
    vl->dsc=calloc(d,4); vl->dph=calloc(d,4); vl->dttl=calloc(d,4);
    vl->dvar=calloc(d,1); vl->dclip=calloc(d,1); vl->dout=calloc(d,1);
    vl->dtr=calloc(d,1); vl->dtg=calloc(d,1); vl->dtb=calloc(d,1); vl->dactive=calloc(d,1);
    return vl;
}
VatLayer *vat_layer_create(const char *meta_path, int max_slots){
    return vat_layer_create_multi(&meta_path, 1, max_slots);
}
void vat_layer_destroy(VatLayer *vl){ if(!vl)return;
    free(vl->seen);free(vl->hx);free(vl->hy);free(vl->spd);free(vl->phaseA);free(vl->phaseB);
    free(vl->blendF);free(vl->hmul);free(vl->clipA);free(vl->clipB);free(vl->pin);free(vl->state);free(vl->target);
    free(vl->blending);free(vl->outfit);free(vl->var);free(vl->tr);free(vl->tg);free(vl->tb);
    free(vl->osT);free(vl->osPh);free(vl->osClip);
    free(vl->dx);free(vl->dy);free(vl->dz);free(vl->dhd);free(vl->dsc);free(vl->dph);free(vl->dttl);
    free(vl->dvar);free(vl->dclip);free(vl->dout);free(vl->dtr);free(vl->dtg);free(vl->dtb);free(vl->dactive);
    free(vl); }
int vat_layer_nvariants(const VatLayer *vl){ return vl->nvar; }
const VatMeta *vat_layer_meta_variant(const VatLayer *vl, int variant){
    if(variant<0||variant>=vl->nvar)variant=0; return &vl->m[variant]; }
const VatMeta *vat_layer_meta(const VatLayer *vl){ return &vl->m[0]; }

/* sceglie l'indice di CLIP per (body, stato) tra le varianti di quel gruppo */
static int pick_clip(VatLayer *vl,int body,int slot,int st){
    int n=vl->group_n[body][st]; if(n<=0) return 0;
    return vl->group_idx[body][st][ hashu(slot*131u+st*977u) % (unsigned)n ]; }

void vat_layer_pin_variant(VatLayer *vl,int slot,int variant){
    if(slot<0||slot>=vl->max||variant<0||variant>=vl->nvar) return;
    vl->pin[slot]=variant+1; }
void vat_layer_set_random_count(VatLayer *vl,int n){
    if(n<1)n=1; if(n>vl->nvar)n=vl->nvar; vl->nrandom=n; }
void vat_layer_set_variant(VatLayer *vl,int slot,int variant){
    if(slot<0||slot>=vl->max||variant<0||variant>=vl->nvar) return;
    if(vl->var[slot]==(unsigned char)variant) return;   /* keep animating */
    vl->var[slot]=(unsigned char)variant;
    vl->state[slot]=ST_WALK; vl->blending[slot]=0;
    vl->clipA[slot]=pick_clip(vl,variant,slot,ST_WALK); }

/* One-shot HIT flinch su un agente vivo. No-op se il body non ha clip hit
   (es. crawler) o se un flinch e' gia' in corso (i colpi ravvicinati non
   ribattono il timer all'infinito). */
void vat_layer_hit(VatLayer *vl, int slot){
    if(slot<0||slot>=vl->max) return;
    int body=vl->var[slot];
    if(vl->group_n[body][ST_HIT]<=0) return;
    if(vl->osT[slot]>0) return;                         /* gia' in flinch */
    vl->osClip[slot]=pick_clip(vl,body,slot,ST_HIT);
    vl->osPh[slot]=0;
    vl->osT[slot]=vl->m[body].clip[vl->osClip[slot]].duration;
    if(vl->osT[slot]<=0) vl->osT[slot]=1.0f;
}

/* Spawna un decedente: una visuale renderer-side che riproduce la clip morte del
   body una volta a (x,y) e poi tiene l'ultimo frame, indipendente dall'agente
   sim gia' rimosso. Snapshot di heading/variante/outfit/tinta dallo stato dello
   slot. Da chiamare alla morte LEGGERA (non gib), prima che lo slot sia riusato.
   No-op se il body non ha clip morte. Ring buffer: pieno = sovrascrive il piu'
   vecchio (come i cadaveri M3.3). */
void vat_layer_die(VatLayer *vl, int slot, float x, float y, float radius){
    if(slot<0||slot>=vl->max) return;
    int body=vl->var[slot];
    /* preferisci 'death'/'dying' (a caso per varieta'); fallback: niente clip */
    int st = (vl->group_n[body][ST_DEATH]>0 && (hashu(slot*7u)&1)) ? ST_DEATH
           : (vl->group_n[body][ST_DYING]>0 ? ST_DYING
           : (vl->group_n[body][ST_DEATH]>0 ? ST_DEATH : -1));
    if(st<0) return;
    int d=vl->dwrite; vl->dwrite=(d+1)%vl->dmax;
    vl->dx[d]=x; vl->dy[d]=y; vl->dz[d]=0.0f; vl->dhd[d]=atan2f(vl->hx[slot],vl->hy[slot])+HEADING_OFFSET;
    vl->dsc[d]=(radius/0.30f)*vl->hmul[slot]; vl->dph[d]=0.0f;
    vl->dvar[d]=(unsigned char)body; vl->dclip[d]=(unsigned char)pick_clip(vl,body,slot,st);
    vl->dout[d]=vl->outfit[slot]; vl->dtr[d]=vl->tr[slot]; vl->dtg[d]=vl->tg[slot]; vl->dtb[d]=vl->tb[slot];
    vl->dttl[d]=vl->m[body].clip[vl->dclip[d]].duration + DECEDENT_HOLD;
    vl->dactive[d]=1;
}

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
    const float *wp=simp_wall_pressure(s);                 /* sensore d'assedio (M5/SIEGE) */
    int n=simp_count(s);
    float alpha=1.0f-expf(-dt/HEADING_TAU), beta=1.0f-expf(-dt/SPEED_TAU);
    /* avanza il pool decessi (indipendente dagli agenti vivi): clip una volta poi
       tiene l'ultimo frame; libera lo slot a TTL scaduto. */
    for(int d=0; d<vl->dmax; d++) if(vl->dactive[d]){
        vl->dttl[d]-=dt; if(vl->dttl[d]<=0.0f){ vl->dactive[d]=0; continue; }
        const VatClip *c=&vl->m[vl->dvar[d]].clip[vl->dclip[d]];
        float dur=c->duration>0?c->duration:1.0f;
        vl->dph[d]+=dt/dur; if(vl->dph[d]>1.0f)vl->dph[d]=1.0f;
    }
    for(int i=0;i<n;i++){
        int slot=simp_slot_of(s,i); if(slot<0||slot>=vl->max) continue;
        SimPHandle h=simp_handle_of(s,i);
        if(vl->seen[slot]!=h){                       /* nuovo agente nello slot */
            unsigned r=hashu(h); float ang=(r&0xffff)*(6.2831853f/65536.0f);
            vl->seen[slot]=h; vl->hx[slot]=sinf(ang)*0.01f; vl->hy[slot]=cosf(ang)*0.01f;
            vl->spd[slot]=1.0f; vl->state[slot]=ST_WALK; vl->blending[slot]=0;
            /* body: pinnato (tipo di gioco, es. crawler) o random fra le cosmetiche */
            int body = vl->pin[slot] ? vl->pin[slot]-1 : (int)(hashu(h+333u)%(unsigned)vl->nrandom);
            vl->pin[slot]=0;                          /* consuma: slot riusato torna libero */
            vl->var[slot]=(unsigned char)body;        /* body model */
            vl->clipA[slot]=pick_clip(vl,body,slot,ST_WALK); vl->phaseA[slot]=(float)((r>>16)&0xff)/256.0f;
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

        int body=vl->var[slot];

        /* one-shot HIT: interrompe la FSM per la durata della clip (tempo, niente
           loop), poi riprende. Heading/spd restano aggiornati sopra. */
        if(vl->osT[slot]>0.0f){
            vl->osT[slot]-=dt;
            const VatClip *c=&vl->m[body].clip[vl->osClip[slot]];
            float dur=c->duration>0?c->duration:1.0f;
            vl->osPh[slot]+=dt/dur; if(vl->osPh[slot]>1.0f)vl->osPh[slot]=1.0f;
            continue;
        }

        /* FSM da velocità (dormienti = idle); gruppi del BODY di questo agente.
           ATTACK = override condizione: l'agente preme un muro/struttura per
           raggiungere il goal oltre (sensore d'assedio wall_pressure). */
        int cur=vl->state[slot];
        int want=(fl[i]&SIMP_DORMANT)?ST_IDLE:want_state(cur,vl->spd[slot]);
        if(wp && wp[i]>ATTACK_PRESS && !(fl[i]&SIMP_FLYING) && vl->group_n[body][ST_ATTACK]>0)
            want=ST_ATTACK;
        int eff=vl->blending[slot]?vl->target[slot]:cur;
        if(want!=eff && vl->group_n[body][want]>0){
            vl->blending[slot]=1; vl->target[slot]=want;
            vl->clipB[slot]=pick_clip(vl,body,slot,want); vl->phaseB[slot]=0; vl->blendF[slot]=0; }

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
    float gA,gB,mix;
    if(vl->osT[slot]>0.0f){                      /* one-shot HIT: gioca una volta, niente loop */
        const VatClip *c=&M->clip[vl->osClip[slot]];
        float local=vl->osPh[slot]*(float)(c->numFrames-1);
        float fa=floorf(local), fb=fa+1; if(fb>=c->numFrames)fb=(float)(c->numFrames-1);
        gA=c->startFrame+fa; gB=c->startFrame+fb; mix=local-fa;
    } else {
        const VatClip *ca=&M->clip[vl->clipA[slot]];
        if(vl->blending[slot]){ const VatClip *cb=&M->clip[vl->clipB[slot]];
            float la=floorf(vl->phaseA[slot]*ca->numFrames), lb=floorf(vl->phaseB[slot]*cb->numFrames);
            gA=ca->startFrame+fmodf(la,(float)ca->numFrames); gB=cb->startFrame+fmodf(lb,(float)cb->numFrames);
            mix=vl->blendF[slot]>1?1:vl->blendF[slot];
        } else { float local=vl->phaseA[slot]*ca->numFrames; float fa=floorf(local),fb=fa+1; if(fb>=ca->numFrames)fb=0;
            gA=ca->startFrame+fa; gB=ca->startFrame+fb; mix=local-fa; }
    }
    float head=atan2f(vl->hx[slot],vl->hy[slot])+HEADING_OFFSET;
    o[0]=x; o[1]=z; o[2]=y; o[3]=head;
    o[4]=(r/0.30f)*vl->hmul[slot]; o[5]=gA; o[6]=gB; o[7]=mix; o[8]=(float)vl->outfit[slot];
    o[9]=vl->tr[slot]/255.0f; o[10]=vl->tg[slot]/255.0f; o[11]=vl->tb[slot]/255.0f;
}

/* Emette un decedente del pool (clip morte una volta, ultimo frame tenuto). */
static void emit_decedent(VatLayer *vl, int d, const VatMeta *M, float *o){
    const VatClip *c=&M->clip[vl->dclip[d]];
    float local=vl->dph[d]*(float)(c->numFrames-1);
    float fa=floorf(local), fb=fa+1; if(fb>=c->numFrames)fb=(float)(c->numFrames-1);
    o[0]=vl->dx[d]; o[1]=vl->dz[d]; o[2]=vl->dy[d]; o[3]=vl->dhd[d];
    o[4]=vl->dsc[d]; o[5]=c->startFrame+fa; o[6]=c->startFrame+fb; o[7]=local-fa;
    o[8]=(float)vl->dout[d]; o[9]=vl->dtr[d]/255.0f; o[10]=vl->dtg[d]/255.0f; o[11]=vl->dtb[d]/255.0f;
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
    /* decedenti di questa variante, in coda agli agenti vivi (stessa mesh/draw) */
    for(int d=0; d<vl->dmax && c<max_inst; d++)
        if(vl->dactive[d] && vl->dvar[d]==variant){ emit_decedent(vl,d,M,buf+c*12); c++; }
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
