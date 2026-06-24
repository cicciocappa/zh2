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
    float vsmin[VAT_MAX_VARIANTS], vsmax[VAT_MAX_VARIANTS]; /* scala fissa per variante (0 = da raggio) */
    int max;
    SimPHandle *seen;
    float *hx, *hy, *spd, *phaseA, *phaseB, *blendF, *hmul;
    int   *clipA, *clipB, *pin;              /* pin[slot] = variante+1 forzata, 0 = libera */
    int   *force_clip;                       /* override clip locomozione, -1 = AUTO (FSM) */
    unsigned char *state, *target, *blending, *outfit, *var, *tr, *tg, *tb;
    /* one-shot HIT overlay (interrompe la FSM per la durata della FINESTRA, poi
       torna). Si riproduce solo un sotto-intervallo casuale della clip hit
       ([osF0, osF0+osLen) frame): flinch piu' corto + varieta' (ogni colpo una
       fetta diversa). hit_ctr desincronizza colpi sullo stesso slot. */
    float *osT, *osPh; int *osClip, *osF0, *osLen;   /* osT[slot]>0 = flinch in corso */
    unsigned hit_ctr;
    /* pool decessi renderer-side: visuali che riproducono la clip morte una volta
       e poi tengono l'ultimo frame, indipendenti dall'agente sim gia' rimosso. */
    int dmax, dwrite;
    float *dx, *dy, *dz, *dhd, *dsc, *dph, *dttl;
    unsigned char *dvar, *dclip, *dout, *dtr, *dtg, *dtb, *dactive;
    /* pool gib (GFX §5): pezzi frattaglia balistici renderer-side alle morti
       esplosive (gib pesante). Solo visuali, niente collisione (come M3.2):
       parabola + spin, atterrano e si posano fino al TTL. z = altezza-sul-suolo
       (il render aggiunge terrain_z). Ring buffer come i decedenti. */
    int gmax, gwrite;
    float *gx, *gy, *gz, *gvx, *gvy, *gvz, *gttl, *ghx, *ghy, *ghz, *gang, *gspin;
    unsigned char *gr, *gg, *gb, *gact;
    /* pool mesh-gib (gore con mesh 3D vere, "maim arm"): come i gib ma con mesh_id
       e tumble a 3 assi (asse normalizzato + angolo + spin) invece di box+yaw. */
    int mgmax, mgwrite;
    float *mgx, *mgy, *mgz, *mgvx, *mgvy, *mgvz, *mgttl, *mgsc;
    float *mgax, *mgay, *mgaz, *mgang, *mgspin;
    unsigned char *mgmesh, *mgact;
    /* decal di sangue PERSISTENTI (GFX §5.2): macchie a terra dove un corpo si
       posa (fine vita del decedente) o esplode (gib). Niente decay, niente
       collisione: ring buffer cappato = il "tappeto dell'orrore" resta limitato. */
    int kmax, kwrite, kcount;
    float *kx, *ky, *ksz;
    unsigned char *kr, *kg, *kb;
    /* decal SAGOMA-CADAVERE persistenti: come i decal di sangue ma ORIENTATI e
       per-VARIANTE (il render li disegna come quad texturizzati campionando
       l'atlante di sprite top-down bakati dai modelli VAT). Emessi quando un
       decedente svanisce, sotto/oltre la macchia. Stesso ring buffer cappato. */
    int qmax, qwrite, qcount;
    float *qx, *qy, *qhd, *qsz;
    unsigned char *qvar, *qtr, *qtg, *qtb;
};

static unsigned hashu(unsigned a){ a^=a>>16; a*=2654435761u; a^=a>>13; a*=2246822519u; a^=a>>16; return a; }

/* aggiunge un decal di sangue persistente a (x,y) (ring buffer cappato) */
static void decal_add(VatLayer *vl, float x, float y, float size, unsigned seed){
    unsigned h=hashu(seed*2654435761u + 0xDECA1u);
    int k=vl->kwrite; vl->kwrite=(k+1)%vl->kmax; if(vl->kcount<vl->kmax)vl->kcount++;
    vl->kx[k]=x; vl->ky[k]=y; vl->ksz[k]=size*(0.8f+(h&255)/255.0f*0.5f);
    float r=0.30f+((h>>8)&63)/63.0f*0.20f, g=0.02f+((h>>14)&15)/15.0f*0.03f, b=0.02f; /* rosso sangue scuro */
    vl->kr[k]=(unsigned char)(r*255); vl->kg[k]=(unsigned char)(g*255); vl->kb[k]=(unsigned char)(b*255);
}

/* aggiunge una sagoma-cadavere persistente (ring buffer cappato): posizione,
   heading del corpo alla morte, raggio mondo (scala dell'agente), variante (->
   cella d'atlante) e tinta. */
static void corpse_decal_add(VatLayer *vl, float x, float y, float hd, float sz,
                             unsigned char var, unsigned char tr, unsigned char tg, unsigned char tb){
    int q=vl->qwrite; vl->qwrite=(q+1)%vl->qmax; if(vl->qcount<vl->qmax)vl->qcount++;
    vl->qx[q]=x; vl->qy[q]=y; vl->qhd[q]=hd; vl->qsz[q]=sz;
    vl->qvar[q]=var; vl->qtr[q]=tr; vl->qtg[q]=tg; vl->qtb[q]=tb;
}

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
    vl->force_clip=malloc(n*sizeof(int)); for(int i=0;i<n;i++)vl->force_clip[i]=-1;
    vl->state=calloc(n,1); vl->target=calloc(n,1); vl->blending=calloc(n,1);
    vl->outfit=calloc(n,1); vl->var=calloc(n,1); vl->tr=calloc(n,1); vl->tg=calloc(n,1); vl->tb=calloc(n,1);
    vl->osT=calloc(n,4); vl->osPh=calloc(n,4); vl->osClip=calloc(n,sizeof(int));
    vl->osF0=calloc(n,sizeof(int)); vl->osLen=calloc(n,sizeof(int));
    /* pool decessi: dimensionato a ~min(max,4096) come i cadaveri (M3.3) */
    vl->dmax = max_slots<4096?max_slots:4096; vl->dwrite=0;
    int d=vl->dmax;
    vl->dx=calloc(d,4); vl->dy=calloc(d,4); vl->dz=calloc(d,4); vl->dhd=calloc(d,4);
    vl->dsc=calloc(d,4); vl->dph=calloc(d,4); vl->dttl=calloc(d,4);
    vl->dvar=calloc(d,1); vl->dclip=calloc(d,1); vl->dout=calloc(d,1);
    vl->dtr=calloc(d,1); vl->dtg=calloc(d,1); vl->dtb=calloc(d,1); vl->dactive=calloc(d,1);
    /* pool gib: cap fisso (bursts piccoli, TTL breve) */
    vl->gmax=2048; vl->gwrite=0; int gp=vl->gmax;
    vl->gx=calloc(gp,4); vl->gy=calloc(gp,4); vl->gz=calloc(gp,4);
    vl->gvx=calloc(gp,4); vl->gvy=calloc(gp,4); vl->gvz=calloc(gp,4); vl->gttl=calloc(gp,4);
    vl->ghx=calloc(gp,4); vl->ghy=calloc(gp,4); vl->ghz=calloc(gp,4); vl->gang=calloc(gp,4); vl->gspin=calloc(gp,4);
    vl->gr=calloc(gp,1); vl->gg=calloc(gp,1); vl->gb=calloc(gp,1); vl->gact=calloc(gp,1);
    /* pool mesh-gib: piccolo (pochi hero pieces per maim) */
    vl->mgmax=256; vl->mgwrite=0; int mp=vl->mgmax;
    vl->mgx=calloc(mp,4); vl->mgy=calloc(mp,4); vl->mgz=calloc(mp,4);
    vl->mgvx=calloc(mp,4); vl->mgvy=calloc(mp,4); vl->mgvz=calloc(mp,4);
    vl->mgttl=calloc(mp,4); vl->mgsc=calloc(mp,4);
    vl->mgax=calloc(mp,4); vl->mgay=calloc(mp,4); vl->mgaz=calloc(mp,4);
    vl->mgang=calloc(mp,4); vl->mgspin=calloc(mp,4);
    vl->mgmesh=calloc(mp,1); vl->mgact=calloc(mp,1);
    /* pool decal persistenti (cap fisso, ring buffer) */
    vl->kmax=8192; vl->kwrite=0; vl->kcount=0; int kp=vl->kmax;
    vl->kx=calloc(kp,4); vl->ky=calloc(kp,4); vl->ksz=calloc(kp,4);
    vl->kr=calloc(kp,1); vl->kg=calloc(kp,1); vl->kb=calloc(kp,1);
    /* pool sagome-cadavere (cap fisso, ring buffer) — più piccolo: una sagoma
       texturizzata è più "pesante" all'occhio di un disco, ne bastano meno. */
    vl->qmax=4096; vl->qwrite=0; vl->qcount=0; int qp=vl->qmax;
    vl->qx=calloc(qp,4); vl->qy=calloc(qp,4); vl->qhd=calloc(qp,4); vl->qsz=calloc(qp,4);
    vl->qvar=calloc(qp,1); vl->qtr=calloc(qp,1); vl->qtg=calloc(qp,1); vl->qtb=calloc(qp,1);
    return vl;
}
VatLayer *vat_layer_create(const char *meta_path, int max_slots){
    return vat_layer_create_multi(&meta_path, 1, max_slots);
}
void vat_layer_destroy(VatLayer *vl){ if(!vl)return;
    free(vl->seen);free(vl->hx);free(vl->hy);free(vl->spd);free(vl->phaseA);free(vl->phaseB);
    free(vl->blendF);free(vl->hmul);free(vl->clipA);free(vl->clipB);free(vl->pin);free(vl->force_clip);free(vl->state);free(vl->target);
    free(vl->blending);free(vl->outfit);free(vl->var);free(vl->tr);free(vl->tg);free(vl->tb);
    free(vl->osT);free(vl->osPh);free(vl->osClip);free(vl->osF0);free(vl->osLen);
    free(vl->dx);free(vl->dy);free(vl->dz);free(vl->dhd);free(vl->dsc);free(vl->dph);free(vl->dttl);
    free(vl->dvar);free(vl->dclip);free(vl->dout);free(vl->dtr);free(vl->dtg);free(vl->dtb);free(vl->dactive);
    free(vl->gx);free(vl->gy);free(vl->gz);free(vl->gvx);free(vl->gvy);free(vl->gvz);free(vl->gttl);
    free(vl->ghx);free(vl->ghy);free(vl->ghz);free(vl->gang);free(vl->gspin);
    free(vl->gr);free(vl->gg);free(vl->gb);free(vl->gact);
    free(vl->mgx);free(vl->mgy);free(vl->mgz);free(vl->mgvx);free(vl->mgvy);free(vl->mgvz);
    free(vl->mgttl);free(vl->mgsc);free(vl->mgax);free(vl->mgay);free(vl->mgaz);
    free(vl->mgang);free(vl->mgspin);free(vl->mgmesh);free(vl->mgact);
    free(vl->kx);free(vl->ky);free(vl->ksz);free(vl->kr);free(vl->kg);free(vl->kb);
    free(vl->qx);free(vl->qy);free(vl->qhd);free(vl->qsz);free(vl->qvar);free(vl->qtr);free(vl->qtg);free(vl->qtb);
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
/* Ferita visiva: passa l'outfit alla sua riga insanguinata (atlante 4x8,
   outfit += 16). Idempotente. Vale per qualunque body (tank incluso). */
void vat_layer_make_bloody(VatLayer *vl,int slot){
    if(slot<0||slot>=vl->max) return;
    if(vl->outfit[slot]<16) vl->outfit[slot]=(unsigned char)(vl->outfit[slot]+16);
}
/* Fissa l'outfit di uno slot (atlante 4x8 = 32 celle: 0..13 puliti, +16 =
   versione insanguinata). Controllo diretto per il lab FX; il gioco usa
   make_bloody. Clampato a [0,31]. */
void vat_layer_set_outfit(VatLayer *vl,int slot,int outfit){
    if(slot<0||slot>=vl->max) return;
    if(outfit<0)outfit=0; if(outfit>31)outfit=31;
    vl->outfit[slot]=(unsigned char)outfit;
}
void vat_layer_set_variant_scale(VatLayer *vl,int variant,float smin,float smax){
    if(variant<0||variant>=vl->nvar) return;
    if(smin<=0.0f){ vl->vsmin[variant]=vl->vsmax[variant]=0.0f; return; }
    if(smax<smin)smax=smin; vl->vsmin[variant]=smin; vl->vsmax[variant]=smax; }

/* scala d'istanza per uno slot: fissa per la variante (proporzioni proprie) o
   raggio/0.30 * jitter cosmetico. La scala fissa e' stabile per slot. */
static float inst_scale(const VatLayer *vl,int variant,int slot,float r){
    if(variant>=0 && variant<vl->nvar && vl->vsmin[variant]>0.0f){
        float t=(hashu((unsigned)slot*2654435761u ^ 0x5C1A1Eu)&1023u)/1023.0f;
        return vl->vsmin[variant]+t*(vl->vsmax[variant]-vl->vsmin[variant]);
    }
    return (r/0.30f)*vl->hmul[slot];
}
void vat_layer_set_variant(VatLayer *vl,int slot,int variant){
    if(slot<0||slot>=vl->max||variant<0||variant>=vl->nvar) return;
    if(vl->var[slot]==(unsigned char)variant) return;   /* keep animating */
    vl->var[slot]=(unsigned char)variant;
    /* Un flinch HIT in corso indicizza il layout di clip del VECCHIO body
       (osClip): sul nuovo body (es. il crawler, 3 clip) e' fuori range -> frame
       sballati = posa distorta/congelata finche' osT non scade. Annullalo e
       riparti pulito dalla WALK del nuovo body. */
    vl->osT[slot]=0.0f; vl->force_clip[slot]=-1;   /* l'indice clip era del vecchio layout */
    vl->state[slot]=ST_WALK; vl->blending[slot]=0;
    vl->clipA[slot]=pick_clip(vl,variant,slot,ST_WALK); vl->phaseA[slot]=0.0f; }

/* Forza la clip di locomozione di uno slot (override FSM), -1 = AUTO. clip_idx
   è un indice nel meta del body corrente dello slot. */
void vat_layer_force_clip(VatLayer *vl,int slot,int clip_idx){
    if(slot<0||slot>=vl->max) return;
    if(clip_idx<0){ vl->force_clip[slot]=-1; return; }
    if(clip_idx>=vl->m[vl->var[slot]].nclips) return;
    vl->force_clip[slot]=clip_idx;
}

/* One-shot HIT flinch su un agente vivo. No-op se il body non ha clip hit
   (es. crawler) o se un flinch e' gia' in corso (i colpi ravvicinati non
   ribattono il timer all'infinito). */
float vat_layer_hit(VatLayer *vl, int slot){
    if(slot<0||slot>=vl->max) return 0.0f;
    int body=vl->var[slot];
    if(vl->group_n[body][ST_HIT]<=0) return 0.0f;
    if(vl->osT[slot]>0) return 0.0f;                    /* gia' in flinch */
    int clip=pick_clip(vl,body,slot,ST_HIT);
    const VatClip *c=&vl->m[body].clip[clip];
    int nf=c->numFrames>0?c->numFrames:1;
    /* finestra casuale dentro la clip: start 1..30, lunghezza 20..30 frame
       (la clip hit reale e' ~61 frame: la piena durerebbe ~2 s). hit_ctr da'
       varieta' tra colpi successivi. Clampata ai limiti della clip per body
       con hit corte. */
    unsigned h=hashu((unsigned)slot*131u + vl->hit_ctr*2654435761u + 0x117u); vl->hit_ctr++;
    int f0 = 1 + (int)(h % 30u);
    int len = 20 + (int)((h>>8) % 11u);
    if(f0 >= nf) f0 = nf>1?nf-1:0;
    if(f0+len > nf) len = nf-f0;
    if(len < 1) len = 1;
    float perFrame = (c->duration>0?c->duration:(float)nf/30.0f)/(float)nf;
    vl->osClip[slot]=clip; vl->osF0[slot]=f0; vl->osLen[slot]=len; vl->osPh[slot]=0;
    vl->osT[slot]=len*perFrame;
    if(vl->osT[slot]<=0) vl->osT[slot]=0.1f;
    return vl->osT[slot];
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
    vl->dsc[d]=inst_scale(vl,body,slot,radius); vl->dph[d]=0.0f;
    vl->dvar[d]=(unsigned char)body; vl->dclip[d]=(unsigned char)pick_clip(vl,body,slot,st);
    vl->dout[d]=vl->outfit[slot]; vl->dtr[d]=vl->tr[slot]; vl->dtg[d]=vl->tg[slot]; vl->dtb[d]=vl->tb[slot];
    vl->dttl[d]=vl->m[body].clip[vl->dclip[d]].duration + DECEDENT_HOLD;
    vl->dactive[d]=1;
}

/* spawn di UN pezzo nel pool gib (ring buffer). Tutto renderer-side, niente
   collisione: balistica + spin, atterra e si posa fino al TTL (vat_layer_update). */
static void gib_push(VatLayer *vl, float x, float y, float z,
                     float vx, float vy, float vz, float ang, float spin,
                     float hx, float hy, float hz, float ttl,
                     unsigned char r, unsigned char g, unsigned char b){
    int gi=vl->gwrite; vl->gwrite=(gi+1)%vl->gmax;
    vl->gx[gi]=x; vl->gy[gi]=y; vl->gz[gi]=z;
    vl->gvx[gi]=vx; vl->gvy[gi]=vy; vl->gvz[gi]=vz;
    vl->gang[gi]=ang; vl->gspin[gi]=spin;
    vl->ghx[gi]=hx; vl->ghy[gi]=hy; vl->ghz[gi]=hz; vl->gttl[gi]=ttl;
    vl->gr[gi]=r; vl->gg[gi]=g; vl->gb[gi]=b; vl->gact[gi]=1;
}

/* burst PICCOLO di gore per un colpo non letale (sangue/schizzi su ferita o
   mutilazione): pochi cubetti minuscoli, spinta bassa -> restano vicino. */
void vat_layer_gib_wound(VatLayer *vl, int slot, float x, float y, float radius, unsigned int seed){
    int has=(slot>=0&&slot<vl->max);
    float ztr=has?vl->tr[slot]/255.0f:0.6f, ztg=has?vl->tg[slot]/255.0f:0.5f, ztb=has?vl->tb[slot]/255.0f:0.5f;
    const int N=4;
    for(int k=0;k<N;k++){
        unsigned h=hashu(seed*2654435761u + (unsigned)k*40503u + 0xA17u);
        float a =(h&1023)/1023.0f*6.2831853f;
        float sp=0.6f+((h>>10)&1023)/1023.0f*1.6f;   /* 0.6-2.2 m/s orizz. */
        float up=1.5f+((h>>20)&1023)/1023.0f*2.0f;   /* 1.5-3.5 m/s su       */
        float s =radius*(0.07f+((h>>24)&15)/15.0f*0.05f);  /* cubetto ~2-4 cm */
        float r,gg,b;
        if(((h>>28)&3)!=0){ r=0.45f+((h>>5)&15)/15.0f*0.25f; gg=0.02f+((h>>9)&7)/7.0f*0.06f; b=0.02f; } /* sangue */
        else             { r=ztr*0.7f; gg=ztg*0.7f; b=ztb*0.7f; }                                       /* carne/vestiti */
        gib_push(vl, x,y, radius*0.6f, cosf(a)*sp, sinf(a)*sp, up,
                 (h&255)/255.0f*6.2831853f, (float)((int)((h>>8)&511)-255)/255.0f*14.0f,
                 s,s,s, 0.8f+((h>>16)&255)/255.0f*0.9f,
                 (unsigned char)(r*255),(unsigned char)(gg*255),(unsigned char)(b*255));
    }
    decal_add(vl, x, y, radius*0.5f, seed^0x33u);    /* macchiolina */
}

/* UN arto volante: box MEDIO allungato (placeholder) con arco balistico. Tinta
   carne+sangue. Il chiamante lo invoca 1x (braccio) o 2x (gambe). */
void vat_layer_limb(VatLayer *vl, int slot, float x, float y, float radius, unsigned int seed){
    (void)slot;
    unsigned h=hashu(seed*2654435761u + 0x5EEDu);
    float a =(h&1023)/1023.0f*6.2831853f;
    float sp=1.2f+((h>>10)&1023)/1023.0f*2.5f;       /* 1.2-3.7 m/s */
    float up=2.5f+((h>>20)&1023)/1023.0f*2.5f;       /* 2.5-5.0 m/s */
    float th=radius*0.16f, ln=radius*0.34f;          /* spessore vs lunghezza dell'arto */
    gib_push(vl, x,y, radius*0.7f, cosf(a)*sp, sinf(a)*sp, up,
             (h&255)/255.0f*6.2831853f, (float)((int)((h>>8)&511)-255)/255.0f*9.0f,
             th, ln, th, 1.6f+((h>>16)&255)/255.0f*1.4f,
             (unsigned char)(0.55f*255),(unsigned char)(0.18f*255),(unsigned char)(0.15f*255));
}

/* Burst di gib alla morte esplosiva (gib pesante): pezzi balistici renderer-side
   a (x,y). Mescola rosso sangue + tint dello zombie (slot, per vestiti/carne).
   radius = taglia del corpo (scala dei pezzi e dell'esplosione). */
void vat_layer_gib(VatLayer *vl, int slot, float x, float y, float radius, unsigned int seed){
    int has=(slot>=0&&slot<vl->max);
    float ztr=has?vl->tr[slot]/255.0f:0.6f, ztg=has?vl->tg[slot]/255.0f:0.5f, ztb=has?vl->tb[slot]/255.0f:0.5f;
    const int N=12;
    for(int k=0;k<N;k++){
        unsigned h=hashu(seed*2654435761u + (unsigned)k*40503u);
        float a =(h&1023)/1023.0f*6.2831853f;
        float sp=1.5f+((h>>10)&1023)/1023.0f*3.5f;   /* 1.5-5 m/s orizzontale */
        float up=3.0f+((h>>20)&1023)/1023.0f*3.5f;   /* 3-6.5 m/s verso l'alto */
        int g=vl->gwrite; vl->gwrite=(g+1)%vl->gmax;
        vl->gx[g]=x; vl->gy[g]=y; vl->gz[g]=radius*0.8f;
        vl->gvx[g]=cosf(a)*sp; vl->gvy[g]=sinf(a)*sp; vl->gvz[g]=up;
        vl->gang[g]=(h&255)/255.0f*6.2831853f;
        vl->gspin[g]=(float)((int)((h>>8)&511)-255)/255.0f*12.0f;
        vl->gttl[g]=1.2f+((h>>16)&255)/255.0f*1.3f;  /* 1.2-2.5 s */
        float base=radius*0.18f;
        if(k<4){ vl->ghx[g]=base*0.45f; vl->ghz[g]=base*0.45f; vl->ghy[g]=base*2.2f; } /* arto allungato */
        else   { float s=base*(0.5f+((h>>24)&15)/15.0f*0.6f); vl->ghx[g]=s; vl->ghz[g]=s; vl->ghy[g]=s; }
        float r,gg,b;
        if(((h>>28)&3)!=0){ r=0.45f+((h>>5)&15)/15.0f*0.25f; gg=0.02f+((h>>9)&7)/7.0f*0.06f; b=0.02f; } /* 3/4 sangue */
        else             { r=ztr*0.7f; gg=ztg*0.7f; b=ztb*0.7f; }                                       /* 1/4 vestiti/carne */
        vl->gr[g]=(unsigned char)(r*255); vl->gg[g]=(unsigned char)(gg*255); vl->gb[g]=(unsigned char)(b*255);
        vl->gact[g]=1;
    }
    decal_add(vl, x, y, radius*1.6f, seed^0x9E37u);   /* pozza all'epicentro dell'esplosione */
}

/* spawn di UN mesh-gib (ring buffer). Tutto renderer-side: balistica + tumble 3D,
   atterra e si posa fino al TTL (vat_layer_update). L'asse viene normalizzato. */
static void mesh_gib_push(VatLayer *vl, int mesh, float x, float y, float z,
                          float vx, float vy, float vz, float scale,
                          float ax, float ay, float az, float spin, float ttl){
    int g=vl->mgwrite; vl->mgwrite=(g+1)%vl->mgmax;
    vl->mgmesh[g]=(unsigned char)mesh;
    vl->mgx[g]=x; vl->mgy[g]=y; vl->mgz[g]=z;
    vl->mgvx[g]=vx; vl->mgvy[g]=vy; vl->mgvz[g]=vz; vl->mgsc[g]=scale;
    float l=sqrtf(ax*ax+ay*ay+az*az); if(l<1e-5f){ ax=0; ay=0; az=1; l=1; }
    vl->mgax[g]=ax/l; vl->mgay[g]=ay/l; vl->mgaz[g]=az/l;
    vl->mgang[g]=0.0f; vl->mgspin[g]=spin; vl->mgttl[g]=ttl; vl->mgact[g]=1;
}

/* Maim braccio: lancia il braccio reciso (mesh 0) + 2 frammenti (mesh 1,2) verso
   il lato del corpo (heading±90°, dove c'era la spalla) con arco balistico, scala
   ~ taglia del corpo e tumble 3D casuale. Lascia una macchiolina all'origine. */
void vat_layer_maim_arm(VatLayer *vl, int slot, float x, float y, float radius,
                        float heading, unsigned int seed){
    (void)slot;
    float scale = radius/0.30f;                 /* radius default 0.30 -> scala 1 */
    float zsh = radius*4.0f;                     /* quota spalla ~1.2 m            */
    for(int k=0;k<3;k++){
        unsigned h=hashu(seed*2654435761u + (unsigned)k*40503u + 0x4A1Bu);
        float side = (k==0) ? 1.0f : ((h&1)?1.0f:-1.0f);
        float a  = heading + side*1.5708f + ((int)((h>>1)&255)-128)/128.0f*0.6f;
        float sp = (k==0?2.5f:3.0f) + ((h>>9)&255)/255.0f*2.0f;    /* orizzontale */
        float up = (k==0?3.0f:3.5f) + ((h>>17)&255)/255.0f*2.0f;   /* verso l'alto*/
        float ax=((int)((h>>3)&255)-128)/128.0f, ay=((int)((h>>11)&255)-128)/128.0f,
              az=((int)((h>>19)&255)-128)/128.0f;
        float spin=(k==0?6.0f:9.0f)+((h>>24)&15)/15.0f*4.0f; if(h&0x40000000u) spin=-spin;
        mesh_gib_push(vl, (k==0?0:k), x, y, zsh, cosf(a)*sp, sinf(a)*sp, up,
                      scale, ax,ay,az, spin, 1.8f+((h>>16)&255)/255.0f*1.2f);
    }
    decal_add(vl, x, y, radius*0.7f, seed^0x1A2Bu);
}

/* Emette i mesh-gib attivi: 9 float/pezzo (mesh_id, x, y, z_sul_suolo, ax, ay, az,
   angle, scale). Il chiamante somma terrain_z(x,y) a z e costruisce la matrice. */
int vat_layer_fill_mesh_gibs(VatLayer *vl, float *out, int max_out){
    int c=0;
    for(int g=0; g<vl->mgmax && c<max_out; g++) if(vl->mgact[g]){
        float *o=out+c*9;
        o[0]=(float)vl->mgmesh[g];
        o[1]=vl->mgx[g]; o[2]=vl->mgy[g]; o[3]=vl->mgz[g];
        o[4]=vl->mgax[g]; o[5]=vl->mgay[g]; o[6]=vl->mgaz[g];
        o[7]=vl->mgang[g]; o[8]=vl->mgsc[g];
        c++;
    }
    return c;
}

/* Emette i decal attivi: 6 float/decal (x, y, size, r, g, b). Il chiamante
   somma terrain_z(x,y) e disegna un disco blended a terra. Ritorna il numero. */
int vat_layer_fill_decals(VatLayer *vl, float *out, int max_out){
    int c=0;
    for(int k=0; k<vl->kcount && c<max_out; k++){
        float *o=out+c*6;
        o[0]=vl->kx[k]; o[1]=vl->ky[k]; o[2]=vl->ksz[k];
        o[3]=vl->kr[k]/255.0f; o[4]=vl->kg[k]/255.0f; o[5]=vl->kb[k]/255.0f;
        c++;
    }
    return c;
}

/* Emette le sagome-cadavere attive: 8 float/decal (x, y, heading, size, variante,
   r, g, b). Il chiamante somma terrain_z(x,y), orienta il quad per heading e
   campiona la cella d'atlante della variante. Ritorna il numero. */
int vat_layer_fill_corpse_decals(VatLayer *vl, float *out, int max_out){
    int c=0;
    for(int q=0; q<vl->qcount && c<max_out; q++){
        float *o=out+c*8;
        o[0]=vl->qx[q]; o[1]=vl->qy[q]; o[2]=vl->qhd[q]; o[3]=vl->qsz[q];
        o[4]=(float)vl->qvar[q];
        o[5]=vl->qtr[q]/255.0f; o[6]=vl->qtg[q]/255.0f; o[7]=vl->qtb[q]/255.0f;
        c++;
    }
    return c;
}

/* Emette i gib attivi: 10 float/pezzo (x, y, z_sul_suolo, hx, hy, hz, ang, r,g,b).
   Il chiamante somma terrain_z(x,y) a z e costruisce un box per pezzo. */
int vat_layer_fill_gibs(VatLayer *vl, float *out, int max_out){
    int c=0;
    for(int g=0; g<vl->gmax && c<max_out; g++) if(vl->gact[g]){
        float *o=out+c*10;
        o[0]=vl->gx[g]; o[1]=vl->gy[g]; o[2]=vl->gz[g];
        o[3]=vl->ghx[g]; o[4]=vl->ghy[g]; o[5]=vl->ghz[g]; o[6]=vl->gang[g];
        o[7]=vl->gr[g]/255.0f; o[8]=vl->gg[g]/255.0f; o[9]=vl->gb[g]/255.0f;
        c++;
    }
    return c;
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
        vl->dttl[d]-=dt; if(vl->dttl[d]<=0.0f){       /* fine vita: lascia macchia + sagoma e svanisce */
            decal_add(vl, vl->dx[d], vl->dy[d], vl->dsc[d]*0.55f, (unsigned)d*2654435761u);
            corpse_decal_add(vl, vl->dx[d], vl->dy[d], vl->dhd[d], vl->dsc[d],
                             vl->dvar[d], vl->dtr[d], vl->dtg[d], vl->dtb[d]);
            vl->dactive[d]=0; continue; }
        const VatClip *c=&vl->m[vl->dvar[d]].clip[vl->dclip[d]];
        float dur=c->duration>0?c->duration:1.0f;
        vl->dph[d]+=dt/dur; if(vl->dph[d]>1.0f)vl->dph[d]=1.0f;
    }
    /* avanza i gib: balistica + spin; atterrano (z=0) e si posano fino al TTL */
    for(int g=0; g<vl->gmax; g++) if(vl->gact[g]){
        vl->gttl[g]-=dt; if(vl->gttl[g]<=0.0f){ vl->gact[g]=0; continue; }
        vl->gvz[g]-=9.81f*dt;
        vl->gx[g]+=vl->gvx[g]*dt; vl->gy[g]+=vl->gvy[g]*dt; vl->gz[g]+=vl->gvz[g]*dt;
        vl->gang[g]+=vl->gspin[g]*dt;
        if(vl->gz[g]<=0.0f){ vl->gz[g]=0.0f; vl->gvz[g]=0.0f;       /* atterrato: si posa */
            vl->gvx[g]*=0.25f; vl->gvy[g]*=0.25f; vl->gspin[g]*=0.25f; }
    }
    /* avanza i mesh-gib: balistica + tumble 3D; atterrano (z=0) e si posano */
    for(int g=0; g<vl->mgmax; g++) if(vl->mgact[g]){
        vl->mgttl[g]-=dt; if(vl->mgttl[g]<=0.0f){ vl->mgact[g]=0; continue; }
        vl->mgvz[g]-=9.81f*dt;
        vl->mgx[g]+=vl->mgvx[g]*dt; vl->mgy[g]+=vl->mgvy[g]*dt; vl->mgz[g]+=vl->mgvz[g]*dt;
        vl->mgang[g]+=vl->mgspin[g]*dt;
        if(vl->mgz[g]<=0.0f){ vl->mgz[g]=0.0f; vl->mgvz[g]=0.0f;
            vl->mgvx[g]*=0.30f; vl->mgvy[g]*=0.30f; vl->mgspin[g]*=0.30f; }
    }
    for(int i=0;i<n;i++){
        int slot=simp_slot_of(s,i); if(slot<0||slot>=vl->max) continue;
        SimPHandle h=simp_handle_of(s,i);
        if(vl->seen[slot]!=h){                       /* nuovo agente nello slot */
            unsigned r=hashu(h); float ang=(r&0xffff)*(6.2831853f/65536.0f);
            vl->seen[slot]=h; vl->hx[slot]=sinf(ang)*0.01f; vl->hy[slot]=cosf(ang)*0.01f;
            vl->spd[slot]=1.0f; vl->state[slot]=ST_WALK; vl->blending[slot]=0;
            vl->osT[slot]=0.0f; vl->force_clip[slot]=-1;  /* niente flinch/override dal precedente slot */
            /* body: pinnato (tipo di gioco, es. crawler) o random fra le cosmetiche */
            int body = vl->pin[slot] ? vl->pin[slot]-1 : (int)(hashu(h+333u)%(unsigned)vl->nrandom);
            vl->pin[slot]=0;                          /* consuma: slot riusato torna libero */
            vl->var[slot]=(unsigned char)body;        /* body model */
            vl->clipA[slot]=pick_clip(vl,body,slot,ST_WALK); vl->phaseA[slot]=(float)((r>>16)&0xff)/256.0f;
            /* 0..13 casuali; 14=carbonizzato 15=acido riservati (fuoco/acido,
               futuro), assegnati esplicitamente, non a caso. +16 = riga sangue. */
            vl->outfit[slot]=(unsigned char)(hashu(h+777u)%14u);
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
            int nf=c->numFrames>0?c->numFrames:1;
            float perFrame=(c->duration>0?c->duration:(float)nf/30.0f)/(float)nf;
            float dur=vl->osLen[slot]*perFrame;            /* durata della FINESTRA, non della clip */
            vl->osPh[slot]+=dt/(dur>0?dur:1.0f); if(vl->osPh[slot]>1.0f)vl->osPh[slot]=1.0f;
            continue;
        }

        /* override clip (lab FX): scavalca la FSM, l'agente resta su force_clip
           (avanzato per distanza/tempo come una locomozione normale). */
        if(vl->force_clip[slot]>=0 && vl->force_clip[slot]<vl->m[body].nclips){
            if(vl->clipA[slot]!=vl->force_clip[slot]) vl->clipA[slot]=vl->force_clip[slot];
            vl->blending[slot]=0;
        } else {
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
        }

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
    if(vl->osT[slot]>0.0f){                      /* one-shot HIT: solo la finestra [osF0,osF0+osLen) */
        /* HARD CUT walk<->hit (per scelta): un crossfade LERP tra pose molto diverse
           (walk mid-passo vs flinch) distorce la geometria; il salto netto è più
           pulito (la scattosità si copre con gib/sangue). Vedi storia git. */
        const VatClip *c=&M->clip[vl->osClip[slot]];
        int len=vl->osLen[slot]>0?vl->osLen[slot]:1;
        float local=vl->osPh[slot]*(float)(len-1);
        float fa=floorf(local), fb=fa+1; if(fb>=len)fb=(float)(len-1);
        float base=(float)(c->startFrame+vl->osF0[slot]);
        gA=base+fa; gB=base+fb; mix=local-fa;
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
    o[4]=inst_scale(vl,vl->var[slot],slot,r); o[5]=gA; o[6]=gB; o[7]=mix; o[8]=(float)vl->outfit[slot];
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
