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
//            VAT_HORDE_BLAST="frame,x,y[,str,up]" -> host_blast a quel frame
//            (danno+impulso+strutture+cadaveri+FX, non più solo il lancio).
//            VAT_HORDE_BARRICADE="x,y,len[,mass]" / VAT_HORDE_CAR="x,y[,len][,mass]".
//            VAT_HORDE_LURE="w,r,linger" = richiamo da fuoco (default -0.8,8,2.5; w>=0 off).
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
#include "prop_world.h"
#include "mission.h"
#include "destruct.h"
#include "edit_pick.h"
#include "editor.h"
#include "place.h"
#include "anim.h"
#include "bio.h"      // economia biomassa (BIOMASS_DESIGN): usata dalla shell
#include "balance.h"  // tabella di bilanciamento runtime (assets/balance.cfg)
#include "soldier.h"  // soldato giocabile (SOLDIER_DESIGN, LOOP_DESIGN F)
#include "model.h"    // glb skinned + skeletal animation (render del soldato)
#include "audio.h"    // sia sandbox che game (la sandbox ora suona: SFX vetro/boom)
#ifdef GAME_SHELL
// GAME_SHELL (GAME_APP_DESIGN.md): stessa sorgente compilata come eseguibile
// `game` — title/menu/briefing/prep/assalto/debrief sopra il mondo esistente.
//   ./game [campaign.txt]      (il dev tool `vat_horde` resta senza shell)
#include "app.h"
#include "font8.h"
#endif

#define MAXA 60000
#define NT 10                  // numero massimo di torrette (anello demo)
static float inst[MAXA*14];   /* 12 base + 2 tumble (pitch,roll) per istanza VAT */
static float shad[MAXA*4];     // ground shadow instances: xyz center + radius
#define EDOVL_MAXV 8192        // vertici max dell'overlay editor (rects+poly+cursore)
static float edovl[EDOVL_MAXV*9];

// gesto di mira delle torrette (piazzamento DIREZIONALE): LMB tiene = ancora la
// posizione, trascina = orienta il cono, rilascia = piazza. Stato condiviso tra
// gestione eventi e blocco ghost del frame (entrambe le build, shell e nuda).
static int   gTAimOn=0;
static float gTAimX=0.0f, gTAimY=0.0f;   // ancora (posizione della torretta)
static float gTAimFacing=0.0f;           // ultima direzione valida del drag

// settore di mira nell'overlay flat (ghost di piazzamento / hover su torretta):
// due bordi radiali + arco sul raggio esterno + lancetta di direzione.
static int push_aim_cone(float *buf, int v, int maxv, float x, float y,
                         float facing, float half, float R,
                         float r, float g, float b){
    if(half>=3.1f){                       // arco pieno: solo l'anello di gittata
        const int NSEG=64;
        for(int k=0;k<NSEG && v+6<=maxv;k++){
            float a0=(float)k*6.2831853f/NSEG, a1=(float)(k+1)*6.2831853f/NSEG;
            v=ed_push_bar(buf,v,x+cosf(a0)*R,y+sinf(a0)*R,
                          x+cosf(a1)*R,y+sinf(a1)*R,0.30f,r,g,b);
        }
        return v;
    }
    int nseg=(int)(half*2.0f/0.12f)+1; if(nseg<4)nseg=4; if(nseg>64)nseg=64;
    if(v+6<=maxv) v=ed_push_bar(buf,v,x,y,x+cosf(facing-half)*R,
                                y+sinf(facing-half)*R,0.30f,r,g,b);
    if(v+6<=maxv) v=ed_push_bar(buf,v,x,y,x+cosf(facing+half)*R,
                                y+sinf(facing+half)*R,0.30f,r,g,b);
    for(int k=0;k<nseg && v+6<=maxv;k++){
        float a0=facing-half+2.0f*half*(float)k/(float)nseg;
        float a1=facing-half+2.0f*half*(float)(k+1)/(float)nseg;
        v=ed_push_bar(buf,v,x+cosf(a0)*R,y+sinf(a0)*R,
                      x+cosf(a1)*R,y+sinf(a1)*R,0.30f,r,g,b);
    }
    if(v+6<=maxv) v=ed_push_bar(buf,v,x,y,x+cosf(facing)*R*0.35f,
                                y+sinf(facing)*R*0.35f,0.16f,r,g,b);
    return v;
}

// rotte previste (LOOP_DESIGN B): streamline dal centro di una exit integrando
// il flow field COMMITTATO (simp_sample_flow, zero API nuove) ed emessa come
// trattini che marciano verso il goal (phase in metri). Termina su flow nullo
// (muro o cella goal: phi piatto), fuori mondo o cap di passi. Un piazzamento
// in PREP forza il ricalcolo nav -> la rotta si sposta da sola al frame dopo.
#define ROUTE_GAP 2.2f          // passo del pattern trattini (m)
static float gRouteT=0.0f;      // clock dell'animazione (solo UI)
static int push_route_dashes(float *buf, int v, int maxv, const SimP *sim,
                             float x0, float y0, float ww, float wh,
                             float phase, float r, float g, float b){
    const float STEP=0.6f, DASH=1.2f;
    float x=x0, y=y0, dist=0.0f;
    for(int it=0; it<2500 && v+6<=maxv; it++){
        float dx,dy; simp_sample_flow(sim,x,y,&dx,&dy);
        if(dx*dx+dy*dy<1e-6f) break;
        float nx=x+dx*STEP, ny=y+dy*STEP;
        if(nx<0.0f||ny<0.0f||nx>=ww||ny>=wh) break;
        float m=fmodf(dist-phase,ROUTE_GAP); if(m<0.0f) m+=ROUTE_GAP;
        if(m<DASH) v=ed_push_bar(buf,v,x,y,nx,ny,0.35f,r,g,b);
        x=nx; y=ny; dist+=STEP;
    }
    return v;
}

// I body type disponibili (asset bakati in vat/assets/). Texture placeholder: la
// skirt non ha ancora il _diffuse.png -> rende flat-shaded (tintata), corretto.
// Gli ULTIMI due (crawler, tank) sono body "di gioco": non assegnati a caso, solo
// via pin/set_variant. Il tank riusa la diffuse di zombie_man (stesso UVMap).
static const char *PREFIX[] = {
    "assets/zombies/zombie_man", "assets/zombies/zombie_man_obese",
    "assets/zombies/zombie_fem", "assets/zombies/zombie_fem_obese",
    "assets/zombies/zombie_child", "assets/zombies/zombie_fem_skirt",
    "assets/zombies/zombie_maimed_arm",    /* monco di un braccio: tipo di gioco, non cosmetico */
    "assets/zombies/zombie_maimed_legs",   /* crawler: tipo di gioco, non cosmetico */
    "assets/zombies/zombie_tank",          /* tank: tipo di gioco, non cosmetico */
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

// vampa alla bocca (muzzle flash): pochi billboard brillanti additivi, vita
// brevissima, cono stretto nel verso di tiro, senza gravità. Leggera = giallo-
// bianco piccolo; pesante = più grande e arancione (colpo secco del cannone).
static const FxEmitterDef MUZZLE_FLASH_DEF = {
    .count=4, .shape=FX_EMIT_POINT,
    .spawn_radius=0.04f, .spawn_box_z=0.04f,
    .spawn_offset_y_min=-0.03f, .spawn_offset_y_max=0.03f,
    .speed_xy_min=1.0f, .speed_xy_max=3.0f, .speed_y_min=-0.3f, .speed_y_max=0.5f,
    .gravity=0.0f, .drag=6.0f, .lifetime_min=0.03f, .lifetime_max=0.07f,
    .start_scale_min=0.28f, .start_scale_max=0.45f, .end_scale_min=0.06f, .end_scale_max=0.12f,
    .start_color={1.0f,0.95f,0.60f,1.0f}, .end_color={1.0f,0.55f,0.15f,0.0f},
    .color_variant_count=0, .sprite_first=-1, .sprite_last=-1,
    .wind_scale=0.0f, .ground_stop=false, .blend=FX_BLEND_ADD, .rate=20.0f,
};
static const FxEmitterDef MUZZLE_FLASH_HVY_DEF = {
    .count=7, .shape=FX_EMIT_POINT,
    .spawn_radius=0.06f, .spawn_box_z=0.06f,
    .spawn_offset_y_min=-0.05f, .spawn_offset_y_max=0.05f,
    .speed_xy_min=1.5f, .speed_xy_max=4.5f, .speed_y_min=-0.4f, .speed_y_max=0.8f,
    .gravity=0.0f, .drag=5.0f, .lifetime_min=0.04f, .lifetime_max=0.10f,
    .start_scale_min=0.45f, .start_scale_max=0.75f, .end_scale_min=0.10f, .end_scale_max=0.20f,
    .start_color={1.0f,0.80f,0.40f,1.0f}, .end_color={1.0f,0.35f,0.08f,0.0f},
    .color_variant_count=0, .sprite_first=-1, .sprite_last=-1,
    .wind_scale=0.0f, .ground_stop=false, .blend=FX_BLEND_ADD, .rate=20.0f,
};
// getto del lanciafiamme (torrette 2.0): lingue additive che corrono lungo il
// cono di tiro (direzione = t->ang, half_angle stretto in fx_emit), si gonfiano
// e salgono un filo mentre sfumano dal giallo al rosso scuro. Emesso a ogni
// tick di fuoco (fire_period ~0.15 s) -> con vita ~0.8 s il getto è continuo.
static const FxEmitterDef FLAME_JET_DEF = {
    .count=9, .shape=FX_EMIT_POINT,
    .spawn_radius=0.08f, .spawn_box_z=0.08f,
    .spawn_offset_y_min=-0.06f, .spawn_offset_y_max=0.06f,
    .speed_xy_min=11.0f, .speed_xy_max=16.0f, .speed_y_min=0.2f, .speed_y_max=1.2f,
    .gravity=-1.5f, .drag=0.5f, .lifetime_min=0.55f, .lifetime_max=0.85f,
    .start_scale_min=0.22f, .start_scale_max=0.38f, .end_scale_min=0.55f, .end_scale_max=0.95f,
    .start_color={1.0f,0.75f,0.20f,0.95f}, .end_color={0.85f,0.10f,0.02f,0.0f},
    .color_variants={ {1.0f,0.85f,0.30f,0.95f},{1.0f,0.60f,0.10f,0.95f},{0.95f,0.45f,0.08f,0.9f} },
    .color_variant_count=3, .sprite_first=-1, .sprite_last=-1,
    .wind_scale=0.15f, .ground_stop=false, .blend=FX_BLEND_ADD, .rate=20.0f,
};
// fumo del getto: poche palle alpha scure che restano dietro le fiamme e salgono.
static const FxEmitterDef FLAME_JET_SMOKE_DEF = {
    .count=2, .shape=FX_EMIT_POINT,
    .spawn_radius=0.10f, .spawn_box_z=0.10f,
    .spawn_offset_y_min=0.0f, .spawn_offset_y_max=0.15f,
    .speed_xy_min=6.0f, .speed_xy_max=10.0f, .speed_y_min=0.6f, .speed_y_max=1.6f,
    .gravity=-1.2f, .drag=1.2f, .lifetime_min=0.9f, .lifetime_max=1.5f,
    .start_scale_min=0.30f, .start_scale_max=0.50f, .end_scale_min=0.9f, .end_scale_max=1.4f,
    .start_color={0.16f,0.14f,0.12f,0.38f}, .end_color={0.10f,0.10f,0.10f,0.0f},
    .color_variant_count=0, .sprite_first=-1, .sprite_last=-1,
    .wind_scale=0.5f, .ground_stop=false, .blend=FX_BLEND_ALPHA, .rate=20.0f,
};
// getto d'acido: gocce verdi in arco balistico che si posano a terra (splash),
// alpha (liquido, non luce). Piu' veloce e teso del lanciafiamme (range 18 m).
static const FxEmitterDef ACID_JET_DEF = {
    .count=8, .shape=FX_EMIT_POINT,
    .spawn_radius=0.05f, .spawn_box_z=0.05f,
    .spawn_offset_y_min=-0.04f, .spawn_offset_y_max=0.06f,
    .speed_xy_min=15.0f, .speed_xy_max=20.0f, .speed_y_min=0.8f, .speed_y_max=2.0f,
    .gravity=6.0f, .drag=0.15f, .lifetime_min=0.7f, .lifetime_max=1.1f,
    .start_scale_min=0.10f, .start_scale_max=0.18f, .end_scale_min=0.06f, .end_scale_max=0.10f,
    .start_color={0.45f,0.95f,0.20f,0.9f}, .end_color={0.20f,0.55f,0.10f,0.35f},
    .color_variants={ {0.50f,1.0f,0.25f,0.9f},{0.35f,0.85f,0.15f,0.9f},{0.55f,0.95f,0.35f,0.85f} },
    .color_variant_count=3, .sprite_first=-1, .sprite_last=-1,
    .wind_scale=0.1f, .ground_stop=true, .blend=FX_BLEND_ALPHA, .rate=20.0f,
};
// scintilla d'impatto: schegge brillanti che rimbalzano verso il tiratore, un
// filo di gravità, additive. Emessa quando lo streak raggiunge il bersaglio.
static const FxEmitterDef SPARK_DEF = {
    .count=6, .shape=FX_EMIT_POINT,
    .spawn_radius=0.04f, .spawn_box_z=0.04f,
    .spawn_offset_y_min=-0.05f, .spawn_offset_y_max=0.10f,
    .speed_xy_min=2.0f, .speed_xy_max=6.5f, .speed_y_min=1.0f, .speed_y_max=4.0f,
    .gravity=12.0f, .drag=0.4f, .lifetime_min=0.08f, .lifetime_max=0.22f,
    .start_scale_min=0.06f, .start_scale_max=0.13f, .end_scale_min=0.01f, .end_scale_max=0.03f,
    .start_color={1.0f,0.90f,0.55f,1.0f}, .end_color={1.0f,0.40f,0.05f,0.0f},
    .color_variants={ {1.0f,0.95f,0.65f,1.0f},{1.0f,0.70f,0.25f,1.0f},{1.0f,0.55f,0.15f,1.0f} },
    .color_variant_count=3, .sprite_first=-1, .sprite_last=-1,
    .wind_scale=0.0f, .ground_stop=false, .blend=FX_BLEND_ADD, .rate=20.0f,
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
// vetro: molte schegge piccole e veloci, ciano-bianche additive (glint), vita
// corta, sparse su tutta l'altezza dell'anta (spawn_offset_y ampio).
static const FxEmitterDef GLASS_DEBRIS_DEF = {
    .count=30, .shape=FX_EMIT_POINT,
    .spawn_radius=0.20f, .spawn_box_z=0.20f,
    .spawn_offset_y_min=-0.60f, .spawn_offset_y_max=0.60f,
    .speed_xy_min=2.5f, .speed_xy_max=7.0f, .speed_y_min=1.0f, .speed_y_max=4.5f,
    .gravity=9.81f, .drag=0.18f, .lifetime_min=0.4f, .lifetime_max=1.0f,
    .start_scale_min=0.03f, .start_scale_max=0.08f, .end_scale_min=0.008f, .end_scale_max=0.02f,
    .start_color={0.80f,0.92f,1.0f,0.95f}, .end_color={0.60f,0.78f,0.90f,0.0f},
    .color_variants={ {0.85f,0.95f,1.0f,0.95f},{0.70f,0.88f,0.98f,0.85f},
                      {0.95f,0.98f,1.0f,1.0f},{0.75f,0.90f,1.0f,0.90f} },
    .color_variant_count=4, .sprite_first=-1, .sprite_last=-1,
    .wind_scale=0.15f, .ground_stop=true, .blend=FX_BLEND_ADD, .rate=20.0f,
};

// detriti del CRATERE (richiesta utente 2026-07-09): zolle scure di asfalto/
// terra sollevate da ogni host_blast, a prescindere dai prop nel raggio (quelli
// sputano i LORO debris via on_prop_burst). Balistici, si posano a terra;
// piu' grossi e piu' lenti delle schegge metalliche — massa, non scintille.
static const FxEmitterDef BLAST_DEBRIS_DEF = {
    .count=40, .shape=FX_EMIT_POINT,
    .spawn_radius=0.35f, .spawn_box_z=0.35f,
    .spawn_offset_y_min=0.10f, .spawn_offset_y_max=0.50f,
    .speed_xy_min=3.5f, .speed_xy_max=9.0f, .speed_y_min=4.5f, .speed_y_max=9.5f,
    .gravity=9.81f, .drag=0.20f, .lifetime_min=1.1f, .lifetime_max=2.0f,
    .start_scale_min=0.34f, .start_scale_max=0.62f, .end_scale_min=0.16f, .end_scale_max=0.28f,
    .start_color={0.33f,0.27f,0.19f,1.0f}, .end_color={0.18f,0.16f,0.13f,0.7f},
    .color_variants={ {0.40f,0.32f,0.21f,1.0f},{0.22f,0.19f,0.15f,1.0f},
                      {0.45f,0.36f,0.24f,1.0f},{0.16f,0.15f,0.14f,1.0f} },  // terra/asfalto
    .color_variant_count=4, .sprite_first=-1, .sprite_last=-1,
    .wind_scale=0.1f, .ground_stop=true, .blend=FX_BLEND_ALPHA, .rate=20.0f,
};

// esplosione (EXPLOSION_DESIGN.md §6): tre burst sovrapposti al punto di scoppio.
// FLASH = lampo brevissimo additivo bianco-giallo che si allarga; FIREBALL =
// palla arancio→rosso scuro con drag alto (alpha); SMOKE = colonna grigia lenta
// che sale, vita lunga, presa dal vento. Data-only come gli altri preset.
// NB: nel 3/4 top-down l'orda (~1.8 m) e i muri (fino a ~6 m) occludono gli FX a
// terra → l'esplosione va resa ALTA, una colonna che sale e si legge sopra la
// folla. Perciò spawn in quota + forte spinta verso l'alto + scale generose.
static const FxEmitterDef EXPL_FLASH_DEF = {
    .count=18, .shape=FX_EMIT_POINT,
    .spawn_radius=0.30f, .spawn_box_z=0.30f,
    .spawn_offset_y_min=0.30f, .spawn_offset_y_max=1.6f,
    .speed_xy_min=2.0f, .speed_xy_max=7.0f, .speed_y_min=2.0f, .speed_y_max=6.0f,
    .gravity=0.0f, .drag=4.0f, .lifetime_min=0.08f, .lifetime_max=0.20f,
    .start_scale_min=1.0f, .start_scale_max=2.0f, .end_scale_min=0.3f, .end_scale_max=0.6f,
    .start_color={1.0f,0.98f,0.80f,1.0f}, .end_color={1.0f,0.60f,0.20f,0.0f},
    .color_variant_count=0, .sprite_first=-1, .sprite_last=-1,
    .wind_scale=0.0f, .ground_stop=false, .blend=FX_BLEND_ADD, .rate=20.0f,
};
static const FxEmitterDef EXPL_FIREBALL_DEF = {
    .count=34, .shape=FX_EMIT_POINT,
    .spawn_radius=0.45f, .spawn_box_z=0.45f,
    .spawn_offset_y_min=0.20f, .spawn_offset_y_max=2.2f,
    .speed_xy_min=1.5f, .speed_xy_max=5.0f, .speed_y_min=3.0f, .speed_y_max=7.5f,
    .gravity=-1.0f, .drag=1.4f, .lifetime_min=0.35f, .lifetime_max=0.95f,
    .start_scale_min=0.9f, .start_scale_max=1.8f, .end_scale_min=0.4f, .end_scale_max=0.9f,
    .start_color={1.0f,0.70f,0.25f,1.0f}, .end_color={0.35f,0.06f,0.02f,0.0f},
    .color_variants={ {1.0f,0.80f,0.35f,1.0f},{1.0f,0.50f,0.12f,1.0f},{0.85f,0.30f,0.06f,1.0f} },
    .color_variant_count=3, .sprite_first=-1, .sprite_last=-1,
    .wind_scale=0.1f, .ground_stop=false, .blend=FX_BLEND_ADD, .rate=20.0f,
};
static const FxEmitterDef EXPL_SMOKE_DEF = {
    .count=26, .shape=FX_EMIT_POINT,
    .spawn_radius=0.40f, .spawn_box_z=0.40f,
    .spawn_offset_y_min=0.80f, .spawn_offset_y_max=3.0f,
    .speed_xy_min=0.5f, .speed_xy_max=2.2f, .speed_y_min=1.8f, .speed_y_max=4.0f,
    .gravity=-0.6f, .drag=1.1f, .lifetime_min=1.6f, .lifetime_max=3.2f,
    .start_scale_min=1.0f, .start_scale_max=2.0f, .end_scale_min=2.5f, .end_scale_max=4.2f,
    .start_color={0.30f,0.28f,0.26f,0.85f}, .end_color={0.15f,0.14f,0.13f,0.0f},
    .color_variants={ {0.34f,0.32f,0.30f,0.8f},{0.24f,0.23f,0.22f,0.85f},{0.40f,0.37f,0.34f,0.7f} },
    .color_variant_count=3, .sprite_first=-1, .sprite_last=-1,
    .wind_scale=0.5f, .ground_stop=false, .blend=FX_BLEND_ALPHA, .rate=20.0f,
};

// fumo di torretta DISTRUTTA: sbuffi piccoli e continui dal rottame (emitter
// fx_start_emitter a rate basso per TURWRECK_SMOKE_S dopo il crollo), colonna
// sottile che sale — stessa lezione anti-occlusione: nasce già in quota e sale.
static const FxEmitterDef TURWRECK_SMOKE_DEF = {
    .count=2, .shape=FX_EMIT_POINT,
    .spawn_radius=0.20f, .spawn_box_z=0.20f,
    .spawn_offset_y_min=0.45f, .spawn_offset_y_max=0.9f,
    .speed_xy_min=0.1f, .speed_xy_max=0.5f, .speed_y_min=1.0f, .speed_y_max=2.0f,
    .gravity=-0.5f, .drag=0.9f, .lifetime_min=1.2f, .lifetime_max=2.4f,
    .start_scale_min=0.35f, .start_scale_max=0.7f, .end_scale_min=1.2f, .end_scale_max=2.0f,
    .start_color={0.22f,0.21f,0.20f,0.55f}, .end_color={0.13f,0.12f,0.12f,0.0f},
    .color_variants={ {0.28f,0.26f,0.24f,0.5f},{0.18f,0.17f,0.16f,0.6f},{0.33f,0.31f,0.28f,0.45f} },
    .color_variant_count=3, .sprite_first=-1, .sprite_last=-1,
    .wind_scale=0.6f, .ground_stop=false, .blend=FX_BLEND_ALPHA, .rate=5.0f,
};

// impatto a terra dello zombie che cade ma NON muore (M3.2): puff breve e basso
// di polvere + detriti che si sprigiona radialmente dai piedi. Serve a mascherare
// lo stacco volo->camminata (side note utente 2026-07-07). Grani beige-grigi che
// schizzano quasi orizzontali (poco speed_y), si allargano e si posano in fretta.
static const FxEmitterDef LAND_DUST_DEF = {
    .count=14, .shape=FX_EMIT_POINT,
    .spawn_radius=0.18f, .spawn_box_z=0.18f,
    .spawn_offset_y_min=0.0f, .spawn_offset_y_max=0.20f,
    .speed_xy_min=1.2f, .speed_xy_max=3.2f, .speed_y_min=0.4f, .speed_y_max=1.6f,
    .gravity=6.0f, .drag=1.6f, .lifetime_min=0.28f, .lifetime_max=0.65f,
    .start_scale_min=0.12f, .start_scale_max=0.26f, .end_scale_min=0.35f, .end_scale_max=0.6f,
    .start_color={0.62f,0.56f,0.46f,0.75f}, .end_color={0.48f,0.44f,0.38f,0.0f},
    .color_variants={ {0.66f,0.60f,0.50f,0.7f},{0.55f,0.50f,0.42f,0.75f},{0.58f,0.53f,0.45f,0.65f} },
    .color_variant_count=3, .sprite_first=-1, .sprite_last=-1,
    .wind_scale=0.3f, .ground_stop=true, .blend=FX_BLEND_ALPHA, .rate=20.0f,
};

// scoppio di un prop distruttibile -> burst FX nel verso di spinta (cono ~35°).
typedef struct { FxParticles *fx; } DestructCtx;
static void on_prop_burst(int idx, const char *debris, float x, float y, float dir, void *ud){
    (void)idx;
    DestructCtx *c=(DestructCtx*)ud;
    char k = debris ? debris[0] : 'w';                          // g=glass m=metal else wood
    const FxEmitterDef *def; float oy;
    if      (k=='g') { def=&GLASS_DEBRIS_DEF; oy=0.9f;          // vetro: sparso sull'anta
                       au_play(SND_GLASS); }                   // crash di vetri
    else if (k=='m') { def=&METAL_DEBRIS_DEF; oy=0.8f; }
    else             { def=&WOOD_DEBRIS_DEF;  oy=0.4f; }
    float o[3]={x, ter_z(x,y)+oy, y};
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
#ifdef GAME_SHELL
static void host_bio_kill(DefBody body);   // kill -> biomassa (definita con gBio)
#endif
static void on_def_event(void *user, int slot, int i, DefBody body, DefEvent ev){
    SpawnCtx *c=(SpawnCtx*)user; (void)body;
#ifdef GAME_SHELL
    // ogni kill frutta biomassa (BIOMASS_DESIGN §1) — light E gib, la resa
    // per body la mappa host_bio_kill. DoT/blast/mine passano tutte di qui.
    if(ev==DEF_EV_DEATH || ev==DEF_EV_GIB) host_bio_kill(body);
#endif
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
        // smembramento in MESH-parti vere (testa+2 braccia+2 gambe+2 tronchi +
        // cubetti + pozza, niente cadavere): gibs.glb, non i cubetti soli.
        vat_layer_explode(c->vl, slot, x, y, r, 1.0f, seed);
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
    else if(ev==DEF_EV_IGNITE)                                  /* preso fuoco: outfit carbonizzato */
        vat_layer_set_outfit(c->vl, slot, 14);                  /* il fuoco addosso lo fa il poll */
    else if(ev==DEF_EV_ACID)                                    /* corroso: outfit sciolto */
        vat_layer_set_outfit(c->vl, slot, 15);
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

// --- gore mesh-gibs (FX_LAB): mesh di assets/models/gibs.glb (arm/frammenti/gambe) in un
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

// --- turret models (assets/models/{light,heavy}_turret.glb): two named nodes,
// "base" (static mount) and "gun" (yaws to the live aim angle, kicks back on
// fire via the ANIM_TURRET_RECOIL envelope). Baked at load into de-indexed
// triangle soups (pos+normal, node transform and uniform scale applied) and
// stamped per turret into the flat 9-float buffer by build_turret_mesh — a few
// hundred verts per turret, CPU transform is free at these counts. Missing or
// malformed .glb -> the old procedural pillar keeps working as fallback.
#define TURRET_SCALE_DEF 1.0f
#define TUR_DRAW_CAP 256          // mirrors defense.c TURRET_CAP (runtime placement)
typedef struct { float *v; int nv; } TurPart;       // nv verts * 6 floats (pos+nrm)
typedef struct { TurPart base, gun; float muzzle_h, muzzle_x; int ok; } TurretModel;
static TurretModel gTurM[4];                        // indexed by DefTurretKind
static float gTurScale = TURRET_SCALE_DEF;

static int tur_read_part(cgltf_node *nd, TurPart *out, float sc){
    if(!nd->mesh || nd->mesh->primitives_count<1) return 0;
    cgltf_primitive *pr=&nd->mesh->primitives[0];
    if(pr->type!=cgltf_primitive_type_triangles || !pr->indices) return 0;
    cgltf_accessor *pos=NULL,*nrm=NULL;
    for(size_t a=0;a<pr->attributes_count;a++){ cgltf_attribute *at=&pr->attributes[a];
        if(at->type==cgltf_attribute_type_position) pos=at->data;
        else if(at->type==cgltf_attribute_type_normal) nrm=at->data; }
    if(!pos) return 0;
    float M[16]; cgltf_node_transform_world(nd,M);
    size_t ni=pr->indices->count;
    float *v=malloc(ni*6*sizeof(float));
    for(size_t k=0;k<ni;k++){
        cgltf_size ix=cgltf_accessor_read_index(pr->indices,k);
        float P[3]={0,0,0}, N[3]={0,1,0};
        cgltf_accessor_read_float(pos,ix,P,3);
        if(nrm) cgltf_accessor_read_float(nrm,ix,N,3);
        float *o=v+k*6;
        o[0]=(M[0]*P[0]+M[4]*P[1]+M[8]*P[2]+M[12])*sc;
        o[1]=(M[1]*P[0]+M[5]*P[1]+M[9]*P[2]+M[13])*sc;
        o[2]=(M[2]*P[0]+M[6]*P[1]+M[10]*P[2]+M[14])*sc;
        o[3]=M[0]*N[0]+M[4]*N[1]+M[8]*N[2];
        o[4]=M[1]*N[0]+M[5]*N[1]+M[9]*N[2];
        o[5]=M[2]*N[0]+M[6]*N[1]+M[10]*N[2];
    }
    out->v=v; out->nv=(int)ni; return 1;
}
static int load_turret_model(const char *path, TurretModel *tm){
    memset(tm,0,sizeof *tm);
    cgltf_options opt={0}; cgltf_data *data=NULL;
    if(cgltf_parse_file(&opt,path,&data)!=cgltf_result_success){
        fprintf(stderr,"turret: parse fail %s (fallback pilastrino)\n",path); return 0; }
    if(cgltf_load_buffers(&opt,data,path)!=cgltf_result_success){
        fprintf(stderr,"turret: buffers fail %s\n",path); cgltf_free(data); return 0; }
    for(size_t n=0;n<data->nodes_count;n++){ cgltf_node *nd=&data->nodes[n];
        if(!nd->name) continue;
        if(!strcmp(nd->name,"base"))     tur_read_part(nd,&tm->base,gTurScale);
        else if(!strcmp(nd->name,"gun")) tur_read_part(nd,&tm->gun,gTurScale); }
    cgltf_free(data);
    if(!tm->base.nv || !tm->gun.nv){
        fprintf(stderr,"turret: %s manca il nodo base/gun (fallback pilastrino)\n",path);
        free(tm->base.v); free(tm->gun.v); memset(tm,0,sizeof *tm); return 0; }
    // muzzle = tracer origin: mid gun height, barrel tip along local +x
    float ymin=1e30f,ymax=-1e30f,xmax=-1e30f;
    for(int k=0;k<tm->gun.nv;k++){ const float *o=tm->gun.v+k*6;
        if(o[1]<ymin)ymin=o[1]; if(o[1]>ymax)ymax=o[1]; if(o[0]>xmax)xmax=o[0]; }
    tm->muzzle_h=0.5f*(ymin+ymax); tm->muzzle_x=xmax;
    tm->ok=1; return 1;
}
// stamp one part into the flat buffer at (tx, zb, tz), yawed by ca/sa about Y
// (world aim (cos a, sin a) = render (x, ., z)); flat color.
static int tur_emit(float *buf, int c, const TurPart *p, float tx, float tz,
                    float zb, float ca, float sa, float cr, float cg, float cb){
    for(int k=0;k<p->nv;k++){ const float *i=p->v+k*6; float *o=buf+(size_t)(c+k)*9;
        o[0]=tx + i[0]*ca - i[2]*sa;
        o[1]=zb + i[1];
        o[2]=tz + i[0]*sa + i[2]*ca;
        o[3]=i[3]*ca - i[5]*sa; o[4]=i[4]; o[5]=i[3]*sa + i[5]*ca;
        o[6]=cr; o[7]=cg; o[8]=cb; }
    return c+p->nv;
}
// --- base container + mortar model (assets/models/base_and_mortar.glb,
// BASE_DESIGN §3/§5): tre nodi per nome. "container" = il box ISO 20"
// (authored alle misure reali BASE_W/D/H, base su y=0, origine al centro del
// footprint); "mortar_stand" ruota sull'AZIMUT di mira attorno al proprio
// asse verticale; "mortar_carriage" = il tubo, VERTICALE a riposo, che si
// inclina dal verticale verso l'azimut per l'alzo (indicazione utente).
// Parti bakate come soup 6-float world-transformed (tur_read_part); colore =
// baseColorFactor del materiale; perni misurati dai vertici bakati. Glb
// assente/malformato -> resta il box arancio placeholder.
typedef struct {
    TurPart cont, stand, tube;
    float cont_col[3], stand_col[3], tube_col[3];
    float stand_px, stand_pz;                 // asse yaw dello stand (x,z modello)
    float tube_px, tube_py, tube_pz;          // perno base del tubo (spazio modello)
    float tube_len;                           // perno -> bocca lungo il tubo a riposo
    int ok;
} BaseModel;
static BaseModel gBaseM;
// atteggiamento del mortaio: azimut MONDO del colpo e alzo dal verticale.
// Lo slew (inseguimento della mira) vive nella shell; senza shell resta fermo.
static float gMortAz=0.0f, gMortTilt=0.0f;

static void base_part_color(cgltf_node *nd, float col[3]){
    col[0]=col[1]=col[2]=0.65f;
    if(!nd->mesh || nd->mesh->primitives_count<1) return;
    cgltf_primitive *pr=&nd->mesh->primitives[0];
    if(pr->material && pr->material->has_pbr_metallic_roughness){
        const cgltf_float *bc=pr->material->pbr_metallic_roughness.base_color_factor;
        col[0]=bc[0]; col[1]=bc[1]; col[2]=bc[2]; }
}
static int load_base_model(const char *path, BaseModel *bm){
    memset(bm,0,sizeof *bm);
    cgltf_options opt={0}; cgltf_data *data=NULL;
    if(cgltf_parse_file(&opt,path,&data)!=cgltf_result_success){
        fprintf(stderr,"base: parse fail %s (fallback box)\n",path); return 0; }
    if(cgltf_load_buffers(&opt,data,path)!=cgltf_result_success){
        fprintf(stderr,"base: buffers fail %s\n",path); cgltf_free(data); return 0; }
    for(size_t n=0;n<data->nodes_count;n++){ cgltf_node *nd=&data->nodes[n];
        if(!nd->name) continue;
        if(!strcmp(nd->name,"container")){
            tur_read_part(nd,&bm->cont,1.0f);  base_part_color(nd,bm->cont_col); }
        else if(!strcmp(nd->name,"mortar_stand")){
            tur_read_part(nd,&bm->stand,1.0f); base_part_color(nd,bm->stand_col); }
        else if(!strcmp(nd->name,"mortar_carriage")){
            tur_read_part(nd,&bm->tube,1.0f);  base_part_color(nd,bm->tube_col); } }
    cgltf_free(data);
    if(!bm->cont.nv || !bm->stand.nv || !bm->tube.nv){
        fprintf(stderr,"base: %s manca container/mortar_stand/mortar_carriage (fallback box)\n",path);
        free(bm->cont.v); free(bm->stand.v); free(bm->tube.v);
        memset(bm,0,sizeof *bm); return 0; }
    // perni dai vertici bakati: stand = centroide x,z (asse verticale);
    // tubo = centro della base (min y), la bocca è il max y a riposo.
    double sx=0,sz=0; for(int k=0;k<bm->stand.nv;k++){ sx+=bm->stand.v[k*6]; sz+=bm->stand.v[k*6+2]; }
    bm->stand_px=(float)(sx/bm->stand.nv); bm->stand_pz=(float)(sz/bm->stand.nv);
    float ymin=1e30f,ymax=-1e30f; double tx=0,tz=0;
    for(int k=0;k<bm->tube.nv;k++){ const float *o=bm->tube.v+k*6;
        if(o[1]<ymin)ymin=o[1];
        if(o[1]>ymax)ymax=o[1];
        tx+=o[0]; tz+=o[2]; }
    bm->tube_px=(float)(tx/bm->tube.nv); bm->tube_py=ymin; bm->tube_pz=(float)(tz/bm->tube.nv);
    bm->tube_len=ymax-ymin;
    bm->ok=1;
    printf("base: %s ok (%d+%d+%d vert, tubo %.2f m)\n",path,
           bm->cont.nv,bm->stand.nv,bm->tube.nv,(double)bm->tube_len);
    return 1;
}
// stamp di una parte del modello base nel buffer flat 9-float. Pipeline per
// vertice (spazio modello -> mondo): [alzo `beta` attorno al perno del tubo,
// il +Y a riposo piega verso il +X locale] -> [azimut relativo `rel` attorno
// all'asse dello stand] -> yaw del container (ca,sa) -> traslazione (tx,ty,tz)
// con ty = quota della BASE del container (a terra o appeso al cavo).
// tint scurisce il colore della parte (danno). Ritorna il nuovo count.
static int base_emit(float *buf, int c, int maxV, const TurPart *p, const float col[3],
                     float tint, float tx, float ty, float tz, float ca, float sa,
                     int do_yaw, float rel, int do_tilt, float beta){
    if(c+p->nv > maxV) return c;
    float cb=cosf(beta), sb=sinf(beta), cr=cosf(rel), sr=sinf(rel);
    float R=col[0]*tint, G=col[1]*tint, B=col[2]*tint;
    for(int k=0;k<p->nv;k++){ const float *i=p->v+k*6;
        float x=i[0], y=i[1], z=i[2], nx=i[3], ny=i[4], nz=i[5];
        if(do_tilt){                        // rotazione attorno a Z locale: +Y -> -X
            // (-X locale col gruppo già girato di MORT_MODEL_YAW: combinazione
            // verificata a occhio dall'utente 2026-07-08 — stand al fronte E
            // tubo piegato sul bersaglio)
            float lx=x-gBaseM.tube_px, ly=y-gBaseM.tube_py;
            x=gBaseM.tube_px + lx*cb - ly*sb;
            y=gBaseM.tube_py + lx*sb + ly*cb;
            float nx2=nx*cb-ny*sb; ny=nx*sb+ny*cb; nx=nx2;
        }
        if(do_yaw){                         // yaw attorno all'asse dello stand
            float lx=x-gBaseM.stand_px, lz=z-gBaseM.stand_pz;
            x=gBaseM.stand_px + lx*cr - lz*sr;
            z=gBaseM.stand_pz + lx*sr + lz*cr;
            float nx2=nx*cr-nz*sr; nz=nx*sr+nz*cr; nx=nx2;
        }
        float *o=buf+(size_t)c*9; c++;
        o[0]=tx + x*ca - z*sa;
        o[1]=ty + y;
        o[2]=tz + x*sa + z*ca;
        o[3]=nx*ca - nz*sa; o[4]=ny; o[5]=nx*sa + nz*ca;
        o[6]=R; o[7]=G; o[8]=B;
    }
    return c;
}
// il modello completo (container + stand + tubo) in un colpo: yaw = yaw del
// container, az_rel = azimut del mortaio RELATIVO al container, tilt = alzo.
// MORT_MODEL_YAW: il gruppo supporto+tubo è authored col fronte girato di
// 180° rispetto all'azimut di tiro (verificato a occhio 2026-07-08) — la
// rotazione fissa raddrizza il fronte, l'alzo +X locale resta sul bersaglio.
#define MORT_MODEL_YAW 3.14159265f
static int base_model_emit(float *buf, int c, int maxV, float tint,
                           float tx, float ty, float tz, float yaw,
                           float az_rel, float tilt){
    float ca=cosf(yaw), sa=sinf(yaw), rel=az_rel+MORT_MODEL_YAW;
    c=base_emit(buf,c,maxV,&gBaseM.cont, gBaseM.cont_col, tint,tx,ty,tz,ca,sa,0,0,0,0);
    c=base_emit(buf,c,maxV,&gBaseM.stand,gBaseM.stand_col,tint,tx,ty,tz,ca,sa,1,rel,0,0);
    c=base_emit(buf,c,maxV,&gBaseM.tube, gBaseM.tube_col, tint,tx,ty,tz,ca,sa,1,rel,1,tilt);
    return c;
}

// veto editor (§10): non si piazza nulla su una cella-statico (buco palazzo).
static int ter_blocked(float x, float y){ return gTerOn && terrain_hole(&gTer, x, y); }

// --- piazzamento a runtime (PLACEMENT_DESIGN.md): catalogo + veto static ---
static int pl_blocked_host(void *u, float x, float y){ (void)u; return ter_blocked(x,y); }
static const PlItem PL_CAT[] = {
    /* kind        name          cost   w     h    radius  hp     mass   combat: 0 = default | trap: 0 | arc (0 = 90°) */
    { PL_BARRICADE, "Barricata",   50,  4.0f, 1.0f, 0.0f, 300.0f, 30.0f, 0, 0,0,0, 0, 0,0,0,0,0,0, 0 },  /* mass>0 = detriti al crollo */
    { PL_TURRET,    "Torretta",   100,  1.0f, 1.0f, 0.5f,   0.0f,  0.0f, 0, 0,0,0, 0, 0,0,0,0,0,0, 0 },
    { PL_BIN,       "Cassonetto",  20,  0.0f, 0.0f, 0.6f,   0.0f, 12.0f, 0, 0,0,0, 0, 0,0,0,0,0,0, 0 },
    { PL_CAR,       "Auto",        60,  3.0f, 0.0f, 0.6f,   0.0f, 20.0f, 0, 0,0,0, 0, 0,0,0,0,0,0, 0 },
};
#define PL_NCAT ((int)(sizeof(PL_CAT)/sizeof(PL_CAT[0])))

#ifdef GAME_SHELL
// Catalogo v1 del GAME (PREP_UI_DESIGN §2): niente bin/auto (entità di livello,
// §9), barriere prezzate AL METRO (strumento a linea, place.h) con spessore
// 1.0 m (>= 2 celle: niente pinch diagonali nella scala rasterizzata). La
// cancellata è il cover semi-trasparente dell'asse C: opacità 0.3, le torrette
// sparano attraverso (test_cover). Parametri combat a 0 = default di place.c.
/* NON-const: host_apply_balance riscrive costi/hp/combat da balance.cfg a ogni
 * build_world (pl_init tiene il puntatore, quindi le righe patchate valgono
 * anche per un pl_init già fatto). I numeri sotto = default compilati. */
static PlItem PL_CAT_GAME[] = {
    /* kind        name          cost   w     h    radius  hp     mass   kind° range per dmg  opac  trap: trig blastR dmg str up arm | arc°
     * (la colonna "heavy" porta l'intero DefTurretKind: 0/1 legacy, 2 fiamme, 3 acido) */
    { PL_TURRET,    "Leggera",    100,  1.0f, 1.0f, 0.5f,   0.0f,  0.0f, 0,    0,0,0,         0,    0,0,0,0,0,0,  90.0f },
    { PL_TURRET,    "Pesante",    250,  1.0f, 1.0f, 0.5f,   0.0f,  0.0f, 1,    0,0,0,         0,    0,0,0,0,0,0,  90.0f },
    { PL_TURRET,    "Fiamme",     180,  1.0f, 1.0f, 0.5f,   0.0f,  0.0f, 2,    0,0,0,         0,    0,0,0,0,0,0,  90.0f },
    { PL_TURRET,    "Acido",      220,  1.0f, 1.0f, 0.5f,   0.0f,  0.0f, 3,    0,0,0,         0,    0,0,0,0,0,0,  90.0f },
    { PL_BARRICADE, "Barricata",   12,  2.5f, 1.0f, 0.0f, 300.0f, 30.0f, 0,    0,0,0,         0,    0,0,0,0,0,0,  0 },
    { PL_BARRICADE, "Cancellata",  20,  2.5f, 1.0f, 0.0f, 200.0f, 15.0f, 0,    0,0,0,         0.3f, 0,0,0,0,0,0,  0 },
    /* MINA (GAME_PLAN fase D): one-shot a pressione. trigger 1.2 m, blast R 6 /
     * dmg 150, loft 22/0.6, arm 1 s (non ti esplode in mano al piazzamento). */
    { PL_TRAP,      "Mina",        40,  0.0f, 0.0f, 0.3f,   0.0f,  0.0f, 0,    0,0,0,         0,    1.2f,6.0f,150.0f,22.0f,0.6f,1.0f, 0 },
};
#define PL_NCAT_GAME ((int)(sizeof(PL_CAT_GAME)/sizeof(PL_CAT_GAME[0])))

// Registro host dei moduli di barriera a LINEA (PREP_UI_DESIGN §5, verdetto
// visivo 2026-07-05): il render disegna ogni modulo come box RUOTATO lungo la
// linea (geometria dal PlLinePlan al commit); la rasterizzazione per-cella
// resta SOLO per collisione/nav — la scala di celle diventa fisica invisibile.
// gSidMod: id struttura -> 1+indice modulo (0 = non-modulo, render per-cella).
// Gli id struttura crescono monotoni in PREP e l'undo è remove-LAST, quindi
// gPlMod resta ordinato per sid e il trim è dal fondo.
enum { PLMOD_MAX = 192, PLMOD_SIDCAP = 256 };   // >= STRUCT_CAP di defense
typedef struct { int sid; float x, y, ang, len, th; } PlModule;
static PlModule gPlMod[PLMOD_MAX]; static int gPlModN = 0;
static unsigned char gSidMod[PLMOD_SIDCAP];
static void plmod_clear(void){
    for(int i=0;i<gPlModN;i++) gSidMod[gPlMod[i].sid]=0;
    gPlModN=0;
}
// dopo un pl_line_commit riuscito: le nuove strutture sono le ULTIME nmod
static void plmod_record(DefGame *g, const PlLinePlan *lp, float th){
    int base=def_struct_count(g)-lp->nmod;
    for(int m=0;m<lp->nmod;m++){
        int sid=base+m;
        if(gPlModN>=PLMOD_MAX || sid<0 || sid>=PLMOD_SIDCAP) return;
        gPlMod[gPlModN]=(PlModule){sid,lp->mx[m],lp->my[m],lp->ang,lp->mlen[m],th};
        gSidMod[sid]=(unsigned char)(1+gPlModN); gPlModN++;
    }
}
// dopo un pl_undo_pop: butta i record delle strutture rimosse (sid >= count)
static void plmod_trim(DefGame *g){
    int n=def_struct_count(g);
    while(gPlModN>0 && gPlMod[gPlModN-1].sid>=n){
        gSidMod[gPlMod[gPlModN-1].sid]=0; gPlModN--; }
}
#endif

// catalogo prop di decoro (§10 stadio 5b): tipo->mesh+scala+label, render-only.
static PropCatalog gCatalog; static int gCatN = 0;

// assi §6 applicati al mondo vivo (prop_world_apply in build_world): footprint
// nav dei prop solidi, id struttura per gli assediabili (render: scurimento col
// danno, sparizione al crollo, skip dei box grigi struct-cell).
static PropWorld gPropW;

// --- prop glb models (EDITOR_PLAN E5): la colonna mesh del catalogo, caricata
// come triangle soup 9-float (pos+nrm+rgb; colore = baseColorFactor del
// materiale, node transform mondo applicato, posizioni scalate dalla colonna
// scale). File assente/oltre il cap -> placeholder procedurale come prima.
// Texture NON lette (flat color: basta per i placeholder; si aggiunge il path
// texturato quando arrivano i prop veri). Stampati per-istanza nel VBO prop da
// build_prop_mesh (yaw + lean topple), stesso pattern delle torrette.
typedef struct { float *v; int nv; } PropModel;    // nv verts * 9 float
static PropModel gPropM[PROP_MAX_DEFS];
#define PROP_GLB_MAX_VERTS 6000
// Carica un glb come triangle soup 9-float (pos+nrm+rgb): colore =
// baseColorFactor del materiale, transform mondo dei nodi applicato, posizioni
// scalate; tutti i nodi con mesh concatenati. 1 = ok (out->v malloc'd, va
// free'd dal chiamante), 0 = assente/troppo grande -> il chiamante usa un
// placeholder. Condiviso da prop e mina.
static int load_glb_soup(const char *path, const char *label, float scale, PropModel *out){
    out->v=NULL; out->nv=0;
    cgltf_options opt={0}; cgltf_data *data=NULL;
    if(cgltf_parse_file(&opt,path,&data)!=cgltf_result_success){
        fprintf(stderr,"%s: %s assente/illeggibile -> placeholder\n",label,path); return 0; }
    if(cgltf_load_buffers(&opt,data,path)!=cgltf_result_success){
        fprintf(stderr,"%s: buffers fail -> placeholder\n",label);
        cgltf_free(data); return 0; }
    size_t tot=0;
    for(size_t n=0;n<data->nodes_count;n++){ cgltf_node *nd=&data->nodes[n];
        if(!nd->mesh) continue;
        for(size_t p=0;p<nd->mesh->primitives_count;p++){
            cgltf_primitive *pr=&nd->mesh->primitives[p];
            if(pr->type!=cgltf_primitive_type_triangles||!pr->indices) continue;
            tot+=pr->indices->count; } }
    if(!tot || tot>PROP_GLB_MAX_VERTS){
        if(tot) fprintf(stderr,"%s: %d vert > cap %d -> placeholder\n",
                        label,(int)tot,PROP_GLB_MAX_VERTS);
        cgltf_free(data); return 0; }
    float *v=malloc(tot*9*sizeof(float)); size_t c=0;
    for(size_t n=0;n<data->nodes_count;n++){ cgltf_node *nd=&data->nodes[n];
        if(!nd->mesh) continue;
        float M[16]; cgltf_node_transform_world(nd,M);
        for(size_t p=0;p<nd->mesh->primitives_count;p++){
            cgltf_primitive *pr=&nd->mesh->primitives[p];
            if(pr->type!=cgltf_primitive_type_triangles||!pr->indices) continue;
            cgltf_accessor *pos=NULL,*nrm=NULL;
            for(size_t a=0;a<pr->attributes_count;a++){ cgltf_attribute *at=&pr->attributes[a];
                if(at->type==cgltf_attribute_type_position) pos=at->data;
                else if(at->type==cgltf_attribute_type_normal) nrm=at->data; }
            if(!pos) continue;
            float col[3]={0.65f,0.65f,0.65f};
            if(pr->material && pr->material->has_pbr_metallic_roughness){
                const cgltf_float *bc=pr->material->pbr_metallic_roughness.base_color_factor;
                col[0]=bc[0]; col[1]=bc[1]; col[2]=bc[2]; }
            for(size_t k=0;k<pr->indices->count;k++){
                cgltf_size ix=cgltf_accessor_read_index(pr->indices,k);
                float P[3]={0,0,0}, N[3]={0,1,0};
                cgltf_accessor_read_float(pos,ix,P,3);
                if(nrm) cgltf_accessor_read_float(nrm,ix,N,3);
                float *o=v+c*9;
                o[0]=(M[0]*P[0]+M[4]*P[1]+M[8]*P[2]+M[12])*scale;
                o[1]=(M[1]*P[0]+M[5]*P[1]+M[9]*P[2]+M[13])*scale;
                o[2]=(M[2]*P[0]+M[6]*P[1]+M[10]*P[2]+M[14])*scale;
                o[3]=M[0]*N[0]+M[4]*N[1]+M[8]*N[2];
                o[4]=M[1]*N[0]+M[5]*N[1]+M[9]*N[2];
                o[5]=M[2]*N[0]+M[6]*N[1]+M[10]*N[2];
                o[6]=col[0]; o[7]=col[1]; o[8]=col[2];
                c++; } } }
    cgltf_free(data);
    out->v=v; out->nv=(int)c; return 1;
}
static void load_prop_models(const PropCatalog *cat){
    int loaded=0, with_mesh=0;
    for(int i=0;i<cat->n;i++){
        const PropDef *d=&cat->defs[i]; gPropM[i].v=NULL; gPropM[i].nv=0;
        if(!d->mesh[0]) continue;
        with_mesh++;
        char lbl[80]; snprintf(lbl,sizeof lbl,"prop '%s'",d->key);
        if(load_glb_soup(d->mesh,lbl,d->scale,&gPropM[i])) loaded++;
    }
    if(with_mesh) printf("prop mesh: %d/%d glb caricati\n",loaded,with_mesh);
}

// modello della mina (assets/models/landmine.glb): triangle soup condivisa, un
// solo modello statico stampato a terra per ogni trappola viva (build_mine_mesh).
static PropModel gMineM;
static float gMineScale=1.0f;

// torretta DISTRUTTA (assets/models/destroyed_turret.glb): il rottame che resta
// al posto del modello vivo dopo il crollo (build_turret_mesh), stessa soup
// 9-float. Placeholder autorabile (oggi un cubo scuro da gfx/destroyed_turret_make.py).
static PropModel gTurWreckM;

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

#define PROP_VERTS_EACH 60   // placeholder: corpo (30) + montante (30)
// cap fisso del VBO prop: budget medio per istanza (i glb possono superare il
// placeholder; PROP_GLB_MAX_VERTS limita il singolo modello, questo il totale)
#define PROP_MESH_CAP   (SCENE_MAX_PROP*600)
#define PROP_TOPPLE_MAX 1.40f  // ~80 deg di abbattimento a t=1
// build nel buffer del chiamante (>= PROP_MESH_CAP vertici: n_prop è cappato a
// SCENE_MAX_PROP), stato di distruzione opzionale (NULL = tutti intatti, es. in
// EDIT). Ritorna il conteggio vertici. Niente malloc: durante un topple viene
// richiamata a ogni step della sim.
// Tabella di bilanciamento (balance.h): default compilati sovrascritti da
// assets/balance.cfg (o VAT_HORDE_BALANCE=path) a OGNI build_world — si tara
// senza ricompilare. Le env VAT_HORDE_* restano prioritarie sul file.
static Balance gBal;
// default granata/RMB (EXPLOSION_DESIGN §10.4: R=8, D0=180); danno da caduta
// costante v1 (§3.4: il buffer landed dà solo handle, non la v d'impatto → una
// costante ragionevole, si tara a occhio nel sandbox). Ora chiavi mortar.*/
// game.fall_damage di balance.cfg (il colpo di mortaio e l'RMB condividono
// il blast di default).
#define BLAST_R    (gBal.mortar.blast_r)
#define BLAST_DMG  (gBal.mortar.blast_damage)
#define FALL_DMG   (gBal.fall_damage)

// Crateri scorch (EXPLOSION_DESIGN §7 v1): ring buffer host di dischi scuri a
// terra timbrati da host_blast, resi con lo shader dei decal di sangue (colore
// per-istanza nero-marrone). Persistenti per la partita, azzerati al rebuild del
// mondo. Piccolo (64): niente decay, il più vecchio viene sovrascritto.
#define SCORCH_MAX 64
static float gScorch[SCORCH_MAX * 3];             // x, y, radius (world m)
static int   gScorchN = 0, gScorchHead = 0;
static void scorch_add(float x, float y, float r) {
    float *e = &gScorch[gScorchHead * 3];
    e[0] = x; e[1] = y; e[2] = r;
    gScorchHead = (gScorchHead + 1) % SCORCH_MAX;
    if (gScorchN < SCORCH_MAX) gScorchN++;
}

// --- caricatori torretta (BIOMASS_DESIGN §5): default per kind, applicati
// alle torrette NUOVE (scena, anello legacy, piazzamento PREP) da uno scan
// incrementale — il core defense resta legacy (mag_size 0 = infinito). Nel
// GAME (GAME_SHELL) i caricatori sono ON di default: sono il lato consumo
// dell'economia biomassa. Nel sandbox vat_horde restano infiniti salvo env.
// VAT_HORDE_MAG="light,heavy,flame,acid" (0 = infinito), VAT_HORDE_RELOAD=s.
static int   gMagDef[4] = {0,0,0,0};
static float gReloadDef = 12.0f;    // §5 v2: la finestra muta deve far MALE
static int   gTurMagSeen = 0;             // torrette gia' equipaggiate
static void host_init_mag_defaults(void){
#ifdef GAME_SHELL
    // caricatori ON nel GAME: colpi per kind da balance.cfg (turret.*.mag)
    for(int k=0;k<4;k++) gMagDef[k]=gBal.tur[k].mag;
#endif
    gReloadDef=gBal.turret_reload_s;
    if(getenv("VAT_HORDE_MAG"))
        sscanf(getenv("VAT_HORDE_MAG"),"%d,%d,%d,%d",
               &gMagDef[0],&gMagDef[1],&gMagDef[2],&gMagDef[3]);
    if(getenv("VAT_HORDE_RELOAD")) gReloadDef=(float)atof(getenv("VAT_HORDE_RELOAD"));
}
static void host_apply_mags(DefGame *g){
    int nt=def_turret_count(g);
    if(nt<gTurMagSeen) gTurMagSeen=nt;    // undo piazzamento: slot riusabili
    for(int id=gTurMagSeen; id<nt; id++){
        DefTurret *t=def_turret(g,id);
        int mk=(t->kind>=0&&t->kind<4)?t->kind:0;
        if(t->mag_size<=0 && gMagDef[mk]>0){
            t->mag_size=gMagDef[mk]; t->mag=t->mag_size; t->reload_s=gReloadDef; }
    }
    gTurMagSeen=nt;
}

#ifdef GAME_SHELL
// Attacchi speciali: colpo di mortaio (GAME_PLAN fase F). Il colpo si paga in
// BIOMASSA al momento del tiro (BIOMASS_DESIGN v2 §4: niente scorte di colpi —
// BIO_MORTAR_COST dal serbatoio). In ASSALTO M o l'icona in
// barra entra in aiming (una X segue il mouse a terra); LMB programma il colpo,
// che dopo MORTAR_DELAY esplode via host_blast sul punto mirato. Stato qui (serve
// a build_world per il reset); barra/aiming più sotto. La formalizzazione (modulo
// strikes.c + test + biomassa + dispersione) arriva con la gestione base / fase F.
#define MORTAR_DELAY (gBal.mortar.delay)  // tempo di volo (s): arco lobato ben leggibile
#define MORTAR_APEX  22.0f         // altezza extra dell'arco al culmine (m)
#define STRIKE_MAX   8
// Colpo in volo (BASE_DESIGN §3): parte dal container (ox,oy) e ATTERRA su (x,y)
// esattamente allo scadere di ttot; il proiettile è fiction (billboard di scia),
// l'impatto è host_blast. t = tempo residuo; p = 1−t/ttot = progresso 0→1.
static struct { float x, y, ox, oy, t, ttot; int on; } gStrikes[STRIKE_MAX];
static void strike_add(float x, float y, float ox, float oy, float delay) {
    for (int i = 0; i < STRIKE_MAX; i++) if (!gStrikes[i].on) {
        gStrikes[i].x = x; gStrikes[i].y = y; gStrikes[i].ox = ox; gStrikes[i].oy = oy;
        gStrikes[i].t = delay; gStrikes[i].ttot = delay; gStrikes[i].on = 1;
        return; }
}
static int gAimMort = 0; static float gAimX = 0.0f, gAimY = 0.0f;
// gittata del mortaio (BASE_DESIGN §3 + richiesta utente 2026-07-08): oltre
// alla massima c'è una MINIMA (sotto non si può inarcare il tiro — e tiene il
// giocatore dallo spammare colpi sull'assedio a contatto). Fuori gittata il
// click è rifiutato e la X di mira si spegne; in aiming i due anelli sono
// disegnati attorno alla base. Env VAT_HORDE_MORTAR_MIN/MAX.
static float gMortMinR = 12.0f, gMortMaxR = 90.0f;
// COOLDOWN fra un colpo e l'altro (2026-07-14): senza, un serbatoio pieno si
// scarica in una raffica di colpi back-to-back — il costo in biomassa da solo
// non è un freno al RITMO. Il tubo si raffredda: finché gMortCd > 0 il click è
// rifiutato e il pulsante lo dice. Env VAT_HORDE_MORTAR_CD.
static float gMortCdMax = 5.0f, gMortCd = 0.0f;
#endif

// Applica la tabella balance alle manopole host (chiamata da build_world dopo
// balance_load, quindi rieseguita a ogni rebuild: il file si ritara live).
// Le env VAT_HORDE_* vincono sul file, applicate qui sopra i valori caricati.
static void host_apply_balance(void){
#ifdef GAME_SHELL
    // catalogo PREP: righe patchate da balance (identità per nome, stabile)
    for(int i=0;i<PL_NCAT_GAME;i++){ PlItem *it=&PL_CAT_GAME[i];
        if(it->kind==PL_TURRET && it->heavy>=0 && it->heavy<4){
            const BalTurret *bt=&gBal.tur[it->heavy];
            it->cost=bt->cost; it->hp=bt->hp; it->range=bt->range;
            it->fire_period=bt->fire_period; it->damage=bt->damage;
            it->arc_deg=bt->arc_deg;
        } else if(it->kind==PL_BARRICADE && strcmp(it->name,"Cancellata")==0){
            it->cost=gBal.fence.cost; it->hp=gBal.fence.hp;
            it->mass=gBal.fence.mass; it->opacity=gBal.fence.opacity;
        } else if(it->kind==PL_BARRICADE){
            it->cost=gBal.barricade.cost; it->hp=gBal.barricade.hp;
            it->mass=gBal.barricade.mass;
        } else if(it->kind==PL_TRAP){
            it->cost=gBal.mine.cost; it->trig_r=gBal.mine.trigger_r;
            it->blast_r=gBal.mine.blast_r; it->blast_dmg=gBal.mine.damage;
            it->strength=gBal.mine.strength; it->up_ratio=gBal.mine.up_ratio;
            it->arm_delay=gBal.mine.arm_s;
        }
    }
    gMortMinR=gBal.mortar.min_range; gMortMaxR=gBal.mortar.max_range;
    gMortCdMax=gBal.mortar.cooldown;
    if(getenv("VAT_HORDE_MORTAR_MIN")) gMortMinR=atof(getenv("VAT_HORDE_MORTAR_MIN"));
    if(getenv("VAT_HORDE_MORTAR_MAX")) gMortMaxR=atof(getenv("VAT_HORDE_MORTAR_MAX"));
    if(getenv("VAT_HORDE_MORTAR_CD"))  gMortCdMax=atof(getenv("VAT_HORDE_MORTAR_CD"));
    if(gMortMinR<0) gMortMinR=0;
    if(gMortMaxR<gMortMinR+1.0f) gMortMaxR=gMortMinR+1.0f;
    if(gMortCdMax<0.0f) gMortCdMax=0.0f;
#endif
    host_init_mag_defaults();   // caricatori (BIOMASS §5): balance + env
}

// Esplosione lato host (EXPLOSION_DESIGN.md §3): la verità di gioco (def_blast:
// agenti/strutture/cadaveri, con gli eventi DEF_EV_* che accendono gib+sangue)
// più la fiction — archetipi prop (§4), FX (§6) e cratere scorch (§7 v1).
// Ritorna nonzero se un prop è cambiato (il chiamante ri-uploada la mesh prop).
// Incendi (burn) + annerimento facciate = prossima passata. I prop SOLID
// (bus/edifici/cancellate) li gestisce già def_blast via il pool struttura (o
// sono muri permanenti): qui NON si toccano; il decor NON-solid
// (tavolini/panchine/bidoni…) scoppia se D(d) supera resist.
static int host_blast(DefGame *g, SimP *s, const Scene *sc, const PropCatalog *cat,
                      Destruct *dz, FxParticles *fx, VatLayer *vl,
                      float x, float y, float r, float dmg, float strength, float up) {
    def_blast(g, x, y, r, dmg, strength, up);     // agenti/strutture + cadaveri FISICI
    vat_layer_clear_corpses(vl, x, y, r);         // + sagome VISIVE nel raggio (§3)
    int changed = 0;
    DestructCtx dc = { fx };
    for (int i = 0; i < sc->n_prop; i++) {
        const PropDef *pd = prop_catalog_find(cat, sc->prop[i].key);
        if (pd && pd->solid) continue;                  // muri/strutture: def_blast
        if (destruct_state(dz, i) == DESTRUCT_GONE) continue;
        float ddx = sc->prop[i].x - x, ddy = sc->prop[i].y - y;
        float d = sqrtf(ddx * ddx + ddy * ddy);
        if (d > r) continue;
        float D = dmg * (1.0f - d / r);
        if (pd && D <= pd->resist) continue;            // resiste (es. idrante)
        destruct_force(dz, sc, i, atan2f(ddy, ddx), on_prop_burst, &dc);  // via dal centro
        changed = 1;
    }
    // FX §6: lampo + fireball al suolo, colonna di fumo poco sopra, zolle
    // scure di cratere (asfalto/terra) sollevate dal punto di scoppio.
    float zg = ter_z(x, y);
    float of[3] = { x, zg + 0.4f, y }, os[3] = { x, zg + 0.9f, y };
    fx_emit(fx, of, &EXPL_FLASH_DEF,    0.0f, -1.0f);
    fx_emit(fx, of, &EXPL_FIREBALL_DEF, 0.0f, -1.0f);
    fx_emit(fx, os, &EXPL_SMOKE_DEF,    0.0f, -1.0f);
    fx_emit(fx, of, &BLAST_DEBRIS_DEF,  0.0f, -1.0f);
    scorch_add(x, y, r * 0.6f);                     // cratere annerito (§7 v1)
    au_play(SND_BOOM);
    (void)s;
    return changed;
}

// Ponte trappola->esplosione (GAME_PLAN fase D): traps_update chiama questo quando
// una mina scatta; lo mappiamo su host_blast (la mina riusa la primitiva
// d'esplosione — friendly fire incluso). prop_changed segnala che una mesh prop
// va ri-uploadata. Il core traps non conosce né def_blast né gli FX.
typedef struct { DefGame *g; SimP *s; const Scene *sc; const PropCatalog *cat;
                 Destruct *dz; FxParticles *fx; VatLayer *vl; int prop_changed; } TrapBlastCtx;
static void on_trap_blast(void *user, int id, float x, float y,
                          float r, float dmg, float strength, float up){
    (void)id;
    TrapBlastCtx *c=user;
    if(host_blast(c->g,c->s,c->sc,c->cat,c->dz,c->fx,c->vl,x,y,r,dmg,strength,up))
        c->prop_changed=1;
}

static int build_prop_mesh(const Scene *sc, const PropCatalog *cat,
                           const Destruct *dz, const DefGame *g, float *buf) {
    int c = 0; static int warned = 0;
    for (int i = 0; i < sc->n_prop; i++) {
        int st = dz ? destruct_state(dz, i) : DESTRUCT_INERT;
        if (st == DESTRUCT_GONE) continue;                 // distrutto: sparito
        // §6: struttura assediabile — scurisce col danno, sparisce al crollo
        // (le celle nav le ha già liberate defense). Prop piazzati in EDIT dopo
        // il build_world hanno indice >= gPropW.n: nessuna struttura.
        int sid = (g && i < gPropW.n) ? gPropW.struct_id[i] : -1;
        float dmg = 1.0f;
        if (sid >= 0) {
            if (def_struct_collapsed(g, sid)) continue;
            float frac = def_struct_hp(g, sid) / (def_struct_hp_max(g, sid) + 1e-3f);
            dmg = 0.35f + 0.65f * frac;                    // come i muri di scena
        }
        const SceneProp *pr = &sc->prop[i];
        const PropDef *d = prop_catalog_find(cat, pr->key);
        float sc_m = d ? d->scale : 1.0f;
        float cr,cg,cb;
        if (d) { int idx = (int)(d - cat->defs) & 7; cr=PROP_PAL[idx][0]; cg=PROP_PAL[idx][1]; cb=PROP_PAL[idx][2]; }
        else   { cr=0.90f; cg=0.10f; cb=0.85f; }         // chiave sconosciuta = magenta
        cr*=dmg; cg*=dmg; cb*=dmg;
        float a = pr->rot * 0.01745329f, ca = cosf(a), sa = sinf(a);
        float zb = ter_z(pr->x, pr->y);
        // abbattimento: theta cresce 0..MAX, la faccia alta ruota attorno alla base
        float th = (st==DESTRUCT_TOPPLING) ? destruct_topple_t(dz,i)*PROP_TOPPLE_MAX : 0.0f;
        float st_=sinf(th), ct_=cosf(th);
        float dh = (st==DESTRUCT_TOPPLING) ? destruct_dir(dz,i) : 0.0f;
        float ddx=cosf(dh), ddz=sinf(dh);
        // mesh glb del catalogo (loader E5): stamp yaw + shear di abbattimento
        // (la quota locale h ruota attorno alla base, come prop_box_lean)
        const PropModel *pm = d ? &gPropM[(int)(d - cat->defs)] : NULL;
        if (pm && pm->nv) {
            if (c + pm->nv > PROP_MESH_CAP) {
                if(!warned){ fprintf(stderr,"prop: VBO pieno (cap %d), istanze oltre non disegnate\n",PROP_MESH_CAP); warned=1; }
                break; }
            for (int k = 0; k < pm->nv; k++) {
                const float *iv = pm->v + (size_t)k*9; float *o = buf + (size_t)(c+k)*9;
                float rx = iv[0]*ca - iv[2]*sa, rz = iv[0]*sa + iv[2]*ca, h = iv[1];
                o[0] = pr->x + rx + h*st_*ddx;
                o[1] = zb + h*ct_;
                o[2] = pr->y + rz + h*st_*ddz;
                o[3] = iv[3]*ca - iv[5]*sa; o[4] = iv[4]; o[5] = iv[3]*sa + iv[5]*ca;
                o[6] = iv[6]*dmg; o[7] = iv[7]*dmg; o[8] = iv[8]*dmg;
            }
            c += pm->nv;
            continue;
        }
        if (c + PROP_VERTS_EACH > PROP_MESH_CAP) {
            if(!warned){ fprintf(stderr,"prop: VBO pieno (cap %d), istanze oltre non disegnate\n",PROP_MESH_CAP); warned=1; }
            break; }
        // corpo: 0.7×0.4 m, alto 0.45 m
        float bH=0.45f*sc_m;
        c = prop_box_lean(buf, c, pr->x, pr->y, 0,0, zb, 0.35f*sc_m, 0.20f*sc_m, bH, ca,sa, cr,cg,cb,
                          bH*st_*ddx, bH*st_*ddz, bH*ct_);
        // montante sul lato frontale (+x locale): 0.1×0.1 m, alto 1.0 m
        float mH=1.0f*sc_m;
        c = prop_box_lean(buf, c, pr->x, pr->y, 0.30f*sc_m,0, zb, 0.06f*sc_m, 0.06f*sc_m, mH, ca,sa,
                          cr*0.8f, cg*0.8f, cb*0.8f, mH*st_*ddx, mH*st_*ddz, mH*ct_);
    }
    return c;
}

// ricarica la mesh prop nel VBO (i prop della Scene cambiano in EDIT / si
// distruggono). Lo store GL è preallocato a PROP_MESH_CAP (DYNAMIC_DRAW) e il
// buffer CPU è uno solo, riusato: durante un topple l'upload gira a 60 Hz.
static int upload_prop_mesh(GLuint vbo, const Scene *sc, const PropCatalog *cat,
                            const Destruct *dz, const DefGame *g){
    static float *buf = NULL;
    if(!buf) buf = malloc((size_t)PROP_MESH_CAP*9*sizeof(float));
    int nv = build_prop_mesh(sc,cat,dz,g,buf);
    glBindBuffer(GL_ARRAY_BUFFER,vbo);
    if(nv) glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)nv*9*sizeof(float),buf);
    return nv;
}

// --- mesh dei draggable (DRAG_DESIGN.md): un cassonetto (box) per oggetto,
// orientato sulla velocità (così si LEGGE che scivola), seatato sul terreno.
// Rebuilt OGNI frame dalle posizioni della sim (gli oggetti si muovono). Stesso
// layout 9-float del flat shader. Restituisce il conteggio vertici.
#define DRAG_VERTS_EACH 30           // prop_box: top(6) + 4 pareti(24)
#define DRAG_DRAW_CAP   256          // cassonetti renderizzati al massimo
#ifdef GAME_SHELL
static Soldier *gSol;                // fwd (definito nella sezione biomassa):
                                     // il suo disco draggable si disegna soldato
#endif
static int gSolMdlOk;                // fwd (sezione soldato): glb skinned caricato
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
    int sol=-1;
#ifdef GAME_SHELL
    if(gSol) sol=soldier_body_index(gSol);   // il corpo del soldato non è un cassonetto
#endif
    for(int i=0;i<nd;i++){
        if(is_car[i]) continue;
        float r=rad[i];
        float a=(vx[i]*vx[i]+vy[i]*vy[i]>1e-4f)?atan2f(vy[i],vx[i]):0.0f;
        float ca=cosf(a),sa=sinf(a), zb=ter_z(px[i],py[i]);
        if(i==sol){   // soldato: modello skinned (draw a parte) o sagoma verde
            if(!gSolMdlOk)
                c=prop_box(buf,c, px[i],py[i], 0,0, zb, r*0.85f, r*0.55f, 1.80f, ca,sa,
                           0.28f,0.42f,0.22f);
            continue;
        }
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

// --- mesh delle torrette: modello .glb base+gun se caricato (il gun segue
// l'angolo di mira e rincula al colpo), altrimenti pilastrino procedurale
// (arancio = leggera, rosso = pesante). Layout 9-float del flat shader
// (pos, normal, color). Rebuilt each frame into a caller buffer: a turret
// sieged to collapse (def_turret_disabled) vanishes. maxV = buffer cap in
// verts. Returns vertex count.
static AnimSys gAnim;          // envelope one-shot dei meccanismi (anim.h): rinculo torrette
// crollo torretta -> one-shot FX (sbuffo + scintille + emitter fumo): il loop
// per-step confronta con lo stato del frame prima. Azzerato in build_world.
static uint8_t gTurWasDead[TUR_DRAW_CAP];
#define TURWRECK_SMOKE_S 25.0f    // durata dell'emitter di fumo sul rottame
static int build_turret_mesh(DefGame *g, float *buf, int maxV){
    int nt=def_turret_count(g); int c=0;
    for(int id=0;id<nt;id++){ DefTurret *t=def_turret(g,id);
        if(def_turret_disabled(g,id)){                     // destroyed: wreck
            float ca=cosf(t->ang), sa=sinf(t->ang), zb=ter_z(t->x,t->y);
            if(gTurWreckM.nv>0){
                if(c+gTurWreckM.nv>maxV) break;
                for(int k=0;k<gTurWreckM.nv;k++){          // yaw come il gun vivo
                    const float *i9=gTurWreckM.v+(size_t)k*9; float *o=buf+(size_t)(c+k)*9;
                    o[0]=t->x + i9[0]*ca - i9[2]*sa;
                    o[1]=zb + i9[1];
                    o[2]=t->y + i9[0]*sa + i9[2]*ca;
                    o[3]=i9[3]*ca - i9[5]*sa; o[4]=i9[4]; o[5]=i9[3]*sa + i9[5]*ca;
                    o[6]=i9[6]; o[7]=i9[7]; o[8]=i9[8]; }
                c+=gTurWreckM.nv;
            } else {                                       // glb assente: moncone scuro
                if(c+36>maxV) break;
                c=prop_box(buf,c, t->x,t->y, 0.0f,0.0f, zb,
                           0.38f,0.38f,0.45f, ca,sa, 0.16f,0.15f,0.14f);
            }
            continue;
        }
        // rinculo: envelope lineare 1->0 di anim.c, v² per il calcio secco.
        float rec=anim_value(&gAnim,ANIM_TURRET_RECOIL,id); rec*=rec;
        float ca=cosf(t->ang), sa=sinf(t->ang);
        int mk=(t->kind>=0&&t->kind<4)?t->kind:0;
        TurretModel *tm=&gTurM[mk];
        if(tm->ok){
            if(c + tm->base.nv + tm->gun.nv > maxV) break;
            float zb=ter_z(t->x,t->y);
            // base fissa; gun sull'angolo di mira, arretrato dal rinculo
            c=tur_emit(buf,c,&tm->base, t->x,t->y, zb, 1.0f,0.0f,
                       0.42f,0.44f,0.48f);
            float k=0.20f*gTurScale*rec;
            // canna tinta per tipo: leggera gialla, pesante rossa,
            // fiamme arancio, acido verde
            float gr=0.95f, gg=0.55f, gb=0.10f;
            if(t->kind==TUR_HEAVY){ gg=0.18f; }
            else if(t->kind==TUR_FLAME){ gr=0.95f; gg=0.35f; gb=0.05f; }
            else if(t->kind==TUR_ACID){ gr=0.30f; gg=0.85f; gb=0.25f; }
            // in RICARICA (BIOMASS §5) la canna si spegne: grigio scuro finche'
            // il countdown non riempie il caricatore (o click = ricarica subito)
            if(def_turret_reloading(g,id)>0.0f){ gr=gg=gb=0.22f; }
            c=tur_emit(buf,c,&tm->gun, t->x-k*ca, t->y-k*sa, zb, ca,sa,
                       gr,gg,gb);
            continue;
        }
        if(c+30 > maxV) break;
        float cx=t->x-0.22f*rec*ca, cz=t->y-0.22f*rec*sa, hw=0.32f;
        float zb=ter_z(cx,cz), h=zb+1.6f;            // seat on terrain
        float cr=0.95f, cg=t->heavy?0.20f:0.55f, cb=0.10f;
        if(def_turret_reloading(g,id)>0.0f){ cr=cg=cb=0.22f; }  // in ricarica
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

// mine piazzate (GAME_PLAN fase D): il modello landmine stampato a terra per
// ogni trappola VIVA (le consumate spariscono, come le torrette distrutte). Senza
// glb un box scuro fa da placeholder. maxV = cap del buffer in verts.
static int build_mine_mesh(const Traps *tr, float *buf, int maxV){
    int c=0, n=traps_count(tr);
    for(int id=0; id<n; id++){
        if(!traps_alive(tr,id)) continue;
        const TrapDef *d=traps_get(tr,id); if(!d) continue;
        float z=ter_z(d->x,d->y);
        if(gMineM.nv>0){
            if(c+gMineM.nv>maxV) break;
            for(int k=0;k<gMineM.nv;k++){ const float *sv=gMineM.v+k*9; float *o=buf+(size_t)(c+k)*9;
                o[0]=d->x+sv[0]; o[1]=z+sv[1]; o[2]=d->y+sv[2];   // no yaw: disco a terra
                o[3]=sv[3]; o[4]=sv[4]; o[5]=sv[5];
                o[6]=sv[6]; o[7]=sv[7]; o[8]=sv[8]; }
            c+=gMineM.nv;
        } else {
            if(c+36>maxV) break;                                  // placeholder: dischetto piatto scuro
            c=prop_box(buf,c, d->x,d->y, 0.0f,0.0f, z, 0.30f,0.30f,0.10f, 1.0f,0.0f, 0.14f,0.14f,0.12f);
        }
    }
    return c;
}

// --- streak dei proiettili: il colpo è hitscan (danno istantaneo in defense.c),
// questo è SOLO la scia cosmetica. Un piccolo pool renderer-side, avanzato nel
// loop a passo fisso come il rinculo. La testa corre dalla bocca al punto
// d'impatto a ~500 m/s; disegnata come segmento GL_LINES additivo con coda che
// sfuma (alpha 0 in coda → alpha 1 in testa = comet). All'arrivo emette la
// scintilla d'impatto. Coord render (x, up=y, z).
#define TRACER_POOL 256
typedef struct { float ox,oy,oz, dx,dy,dz, len, t; int heavy; } Tracer;
static Tracer gTrc[TRACER_POOL]; static int gTrcN=0;
static float gBulletV=500.0f;                 // m/s (VAT_HORDE_BULLET_V)

static void tracer_spawn(float ox,float oy,float oz, float ex,float ey,float ez, int heavy){
    float dx=ex-ox, dy=ey-oy, dz=ez-oz; float L=sqrtf(dx*dx+dy*dy+dz*dz);
    if(L<1e-3f) return;
    if(gTrcN>=TRACER_POOL){                    // pieno: rimpiazza il più vecchio
        int m=0; for(int i=1;i<gTrcN;i++) if(gTrc[i].t>gTrc[m].t) m=i; gTrcN=m; if(gTrcN<0)gTrcN=0; }
    Tracer *tr=&gTrc[gTrcN++];
    tr->ox=ox;tr->oy=oy;tr->oz=oz; tr->dx=dx/L;tr->dy=dy/L;tr->dz=dz/L;
    tr->len=L; tr->t=0.0f; tr->heavy=heavy;
}
// avanza il pool; all'arrivo emette la scintilla e ritira lo slot (swap-pop).
static void tracer_step(FxParticles *fx, float dt){
    float V=gBulletV;
    for(int i=0;i<gTrcN;){
        Tracer *tr=&gTrc[i]; tr->t+=dt;
        if(V*tr->t >= tr->len){
            float ex=tr->ox+tr->dx*tr->len, ey=tr->oy+tr->dy*tr->len, ez=tr->oz+tr->dz*tr->len;
            float o[3]={ex,ey,ez};
            float back=atan2f(-tr->dz,-tr->dx);        // scintilla verso il tiratore
            fx_emit(fx,o,&SPARK_DEF,back,0.7f);
            gTrc[i]=gTrc[--gTrcN];                      // swap-pop
        } else i++;
    }
}
// riempie il VBO dei tracer (2 vert/streak: coda alpha0 → testa alpha1). Layout
// 7 float: pos(3)+rgba(4). Ritorna il numero di VERTICI. buf sized TRACER_POOL*2*7.
static int build_tracer_mesh(float *buf){
    float V=gBulletV; int c=0;
    for(int i=0;i<gTrcN;i++){ Tracer *tr=&gTrc[i];
        float head=V*tr->t; if(head>tr->len) head=tr->len;
        float slen=tr->heavy?6.0f:4.0f; float tail=head-slen; if(tail<0.0f) tail=0.0f;
        float hx=tr->ox+tr->dx*head, hy=tr->oy+tr->dy*head, hz=tr->oz+tr->dz*head;
        float tx=tr->ox+tr->dx*tail, ty=tr->oy+tr->dy*tail, tz=tr->oz+tr->dz*tail;
        // colore caldo: leggera giallo-bianco, pesante arancione
        float cr=1.0f, cg=tr->heavy?0.60f:0.92f, cb=tr->heavy?0.20f:0.55f;
        float *o=buf+c*7; o[0]=tx;o[1]=ty;o[2]=tz; o[3]=cr;o[4]=cg;o[5]=cb;o[6]=0.0f; c++;   // coda: alpha 0
        o=buf+c*7;       o[0]=hx;o[1]=hy;o[2]=hz; o[3]=cr;o[4]=cg;o[5]=cb;o[6]=1.0f; c++;     // testa: alpha 1
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
// BASE container (BASE_DESIGN §3): the lz core is one real container ISO 20"
// (~6,1 × 2,44 × 2,6 m, Chinook hook load), orientabile via lz yaw. Placement
// stored here at build time so build_struct_mesh can draw it as ONE oriented box.
static int gLzCore=-1;                   // id struttura core della LZ (se lz), -1 = nessuna
static float gLzX=0, gLzY=0, gLzYaw=0;   // centro (m) e orientamento (radianti) del container
static float gBaseOX=0, gBaseOY=0;       // origine attacchi speciali: container se lz, else centro base
#define BASE_W 6.1f
#define BASE_D 2.44f
#define BASE_H 2.6f
// container in mano all'elicottero (cinematiche BASE_DESIGN §4): finché è
// appeso al cavo la mesh della base NON si disegna a terra (la struttura sim
// esiste già — l'orda è assente/ferma durante le cinematiche, non importa).
static int gLzHeld=0;

#ifdef GAME_SHELL
// bocca del mortaio in coordinate render (x, up, z=sim y): perno del tubo +
// tube_len lungo il tubo inclinato, poi azimut relativo attorno allo stand,
// yaw del container, e la quota di terra. Fallback senza modello: centro
// container a quota BASE_H.
static void mortar_muzzle(float *ox, float *oy, float *oz){
    float zb=ter_z(gLzX,gLzY);
    if(!gBaseM.ok || gLzCore<0){ *ox=gBaseOX; *oy=ter_z(gBaseOX,gBaseOY)+BASE_H; *oz=gBaseOY; return; }
    float b=gMortTilt, rel=gMortAz-gLzYaw+MORT_MODEL_YAW;
    float x=gBaseM.tube_px - gBaseM.tube_len*sinf(b);   // -X: come il tilt di base_emit
    float y=gBaseM.tube_py + gBaseM.tube_len*cosf(b);
    float z=gBaseM.tube_pz;
    float lx=x-gBaseM.stand_px, lz=z-gBaseM.stand_pz, cr=cosf(rel), sr=sinf(rel);
    x=gBaseM.stand_px + lx*cr - lz*sr; z=gBaseM.stand_pz + lx*sr + lz*cr;
    float ca=cosf(gLzYaw), sa=sinf(gLzYaw);
    *ox=gLzX + x*ca - z*sa; *oy=zb + y; *oz=gLzY + x*sa + z*ca;
}

// --- chinook (assets/models/chinook.glb, BASE_DESIGN §4-§5): consegna ed
// estrazione del container. Nodi per nome: "helicopter_body" (statico),
// "rotor1"/"rotor2" (girano attorno all'asse del proprio disco: perno =
// centroide, asse = normale media delle pale — i nodi sono leggermente
// inclinati, l'asse va misurato, non assunto verticale), "cable" (ancorato
// al suo punto PIÙ ALTO: la scala Y lo srotola verso il basso, indicazione
// utente — il perno del nodo glb non sta in cima, quindi ri-ancoriamo noi).
// Parti = VAO indicizzati 8-float (pos/nrm/uv) per lo shader mesh.vs/fs dei
// mesh-gib; texture = base-color embedded del glb (il cavo, senza materiale,
// usa una 1x1 grigia). Traiettoria e fasi = stato host (heli_update), il glb
// non porta animazioni.
typedef struct { GLuint vao; int ni; float piv[3]; float axis[3]; } HeliPart;
static struct {
    HeliPart body, r1, r2, cable;
    GLuint tex, greyTex;
    float cable_top;                 // quota (modello) dell'ancora del cavo
    float cable_len;                 // lunghezza del cavo a scala 1
    int ok;
} gHeliM;

// legge un nodo come VAO 8-float indicizzato, vertici in spazio mondo del glb.
// out_v/out_n = bounds opzionali. Ritorna il numero di vertici (0 = niente).
static int heli_read_node(cgltf_node *nd, HeliPart *out, float bmin[3], float bmax[3]){
    memset(out,0,sizeof *out);
    if(!nd->mesh || nd->mesh->primitives_count<1) return 0;
    cgltf_primitive *pr=&nd->mesh->primitives[0];
    if(pr->type!=cgltf_primitive_type_triangles || !pr->indices) return 0;
    cgltf_accessor *pos=NULL,*nrm=NULL,*uv=NULL;
    for(size_t a=0;a<pr->attributes_count;a++){ cgltf_attribute *at=&pr->attributes[a];
        if(at->type==cgltf_attribute_type_position) pos=at->data;
        else if(at->type==cgltf_attribute_type_normal) nrm=at->data;
        else if(at->type==cgltf_attribute_type_texcoord && at->index==0) uv=at->data; }
    if(!pos) return 0;
    float M[16]; cgltf_node_transform_world(nd,M);
    size_t nv=pos->count, ni=pr->indices->count;
    float *verts=malloc(nv*8*sizeof(float));
    unsigned short *idx=malloc(ni*sizeof(unsigned short));
    double cx=0,cy=0,cz=0, ax=0,ay=0,az=0;
    for(size_t v=0;v<nv;v++){
        float P[3]={0,0,0}, N[3]={0,1,0}, T[2]={0,0};
        cgltf_accessor_read_float(pos,v,P,3);
        if(nrm) cgltf_accessor_read_float(nrm,v,N,3);
        if(uv)  cgltf_accessor_read_float(uv,v,T,2);
        float wx=M[0]*P[0]+M[4]*P[1]+M[8]*P[2]+M[12];
        float wy=M[1]*P[0]+M[5]*P[1]+M[9]*P[2]+M[13];
        float wz=M[2]*P[0]+M[6]*P[1]+M[10]*P[2]+M[14];
        float nx=M[0]*N[0]+M[4]*N[1]+M[8]*N[2];
        float ny=M[1]*N[0]+M[5]*N[1]+M[9]*N[2];
        float nz=M[2]*N[0]+M[6]*N[1]+M[10]*N[2];
        float *o=verts+v*8; o[0]=wx;o[1]=wy;o[2]=wz; o[3]=nx;o[4]=ny;o[5]=nz; o[6]=T[0];o[7]=T[1];
        cx+=wx; cy+=wy; cz+=wz; ax+=nx; ay+=ny; az+=nz;
        if(bmin){ if(wx<bmin[0])bmin[0]=wx; if(wy<bmin[1])bmin[1]=wy; if(wz<bmin[2])bmin[2]=wz; }
        if(bmax){ if(wx>bmax[0])bmax[0]=wx; if(wy>bmax[1])bmax[1]=wy; if(wz>bmax[2])bmax[2]=wz; }
    }
    for(size_t k=0;k<ni;k++) idx[k]=(unsigned short)cgltf_accessor_read_index(pr->indices,k);
    out->piv[0]=(float)(cx/nv); out->piv[1]=(float)(cy/nv); out->piv[2]=(float)(cz/nv);
    double al=sqrt(ax*ax+ay*ay+az*az);
    if(al>1e-6){ out->axis[0]=(float)(ax/al); out->axis[1]=(float)(ay/al); out->axis[2]=(float)(az/al); }
    else { out->axis[0]=0; out->axis[1]=1; out->axis[2]=0; }
    GLuint vao,vbo,ebo; glGenVertexArrays(1,&vao);glBindVertexArray(vao);
    glGenBuffers(1,&vbo);glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)nv*8*sizeof(float),verts,GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,8*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,0,8*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,2,GL_FLOAT,0,8*sizeof(float),(void*)(6*sizeof(float)));glEnableVertexAttribArray(2);
    glGenBuffers(1,&ebo);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)ni*sizeof(unsigned short),idx,GL_STATIC_DRAW);
    glBindVertexArray(0); free(verts); free(idx);
    out->vao=vao; out->ni=(int)ni;
    return (int)nv;
}
static int heli_load(const char *path){
    memset(&gHeliM,0,sizeof gHeliM);
    cgltf_options opt={0}; cgltf_data *data=NULL;
    if(cgltf_parse_file(&opt,path,&data)!=cgltf_result_success){
        fprintf(stderr,"heli: parse fail %s (cinematiche saltate)\n",path); return 0; }
    if(cgltf_load_buffers(&opt,data,path)!=cgltf_result_success){
        fprintf(stderr,"heli: buffers fail %s\n",path); cgltf_free(data); return 0; }
    char dir[512]; snprintf(dir,sizeof dir,"%s",path);
    char *slash=strrchr(dir,'/'); if(slash) slash[1]='\0'; else dir[0]='\0';
    int nb=0,n1=0,n2=0,nc=0;
    float cmin[3]={1e30f,1e30f,1e30f}, cmax[3]={-1e30f,-1e30f,-1e30f};
    for(size_t n=0;n<data->nodes_count;n++){ cgltf_node *nd=&data->nodes[n];
        if(!nd->name || !nd->mesh) continue;
        if(!strcmp(nd->name,"helicopter_body")) nb=heli_read_node(nd,&gHeliM.body,NULL,NULL);
        else if(!strcmp(nd->name,"rotor1"))     n1=heli_read_node(nd,&gHeliM.r1,NULL,NULL);
        else if(!strcmp(nd->name,"rotor2"))     n2=heli_read_node(nd,&gHeliM.r2,NULL,NULL);
        else if(!strcmp(nd->name,"cable"))      nc=heli_read_node(nd,&gHeliM.cable,cmin,cmax);
        if(!gHeliM.tex && nd->mesh->primitives_count>0)
            gHeliM.tex=ground_load_tex(data,&nd->mesh->primitives[0],dir);
    }
    cgltf_free(data);
    if(!nb || !n1 || !n2 || !nc){
        fprintf(stderr,"heli: %s manca body/rotor1/rotor2/cable (cinematiche saltate)\n",path);
        return 0; }
    gHeliM.cable_top=cmax[1];
    gHeliM.cable_len=cmax[1]-cmin[1];
    // l'ancora del cavo è il suo punto PIÙ ALTO: heli_part_mat scala attorno
    // al pivot (piv + S·(v-piv)), quindi la scala Y lo srotola verso il basso
    // restando attaccato alla pancia.
    gHeliM.cable.piv[1]=cmax[1];
    // 1x1 grigio-cavo per le parti senza texture (mesh.fs campiona sempre uTex)
    unsigned char grey[3]={82,82,88};
    glGenTextures(1,&gHeliM.greyTex); glBindTexture(GL_TEXTURE_2D,gHeliM.greyTex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,1,1,0,GL_RGB,GL_UNSIGNED_BYTE,grey);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    if(!gHeliM.tex) gHeliM.tex=gHeliM.greyTex;
    gHeliM.ok=1;
    printf("heli: %s ok (body %d, rotori %d+%d, cavo %d vert, cavo %.2f m)\n",
           path,nb,n1,n2,nc,(double)gHeliM.cable_len);
    return 1;
}

// --- sequenza (fasi host, BASE_DESIGN §4): IN (entra in quota lungo l'asse
// del container) -> DOWN (scende sopra la lz) -> ACT (deposita+ritira il cavo,
// o srotola+aggancia) -> UP -> OUT (esce oltre il bordo) -> fine = CONFIRM
// alla shell (stesso percorso dello skip con INVIO).
enum { HELI_IN, HELI_DOWN, HELI_ACT, HELI_UP, HELI_OUT };
#define HELI_CRUISE   32.0f       // quota di crociera sopra il terreno (m)
#define HELI_SPEED    26.0f       // velocità orizzontale (m/s)
#define HELI_VSPEED    7.5f       // velocità verticale (m/s)
#define HELI_CABLE_FLY 2.4f       // scala del cavo col carico appeso
#define HELI_CABLE_MIN 0.06f      // cavo ritirato (quasi invisibile)
#define HELI_ROTOR_W  17.0f       // spin dei rotori (rad/s)
#define HELI_CAM_HH   30.0f       // half-height camera per le cinematiche (follow)
#define HELI_CAM_RATE  2.0f       // 1/s: convergenza esponenziale del follow (pos+zoom)
// camera di GIOCO vincolata (decisione 2026-07-08): elevation FISSA (il ¾ di
// GFX_DESIGN — si gioca sempre alla stessa inquadratura), yaw libero (per
// guardare dietro palazzi e muri), zoom clampato (niente close-up sui lowpoly
// né vista-satellite). Vale solo per la shell fuori da EDIT; il sandbox
// vat_horde resta libero. Se l'ombra visiva dietro gli statici alti risulta
// scomoda, alzare l'elevation fissa, non liberarla.
#define GAME_CAM_EL     0.40f     // rad (~23°, il default storico)
#define GAME_CAM_HH_MIN 12.0f     // zoom-in massimo (half-height, m)
#define GAME_CAM_HH_MAX 40.0f     // zoom-out massimo (HELI_CAM_HH ci sta dentro)
static struct {
    int active;                   // 0 spento, 1 = consegna, 2 = estrazione
    int phase; float t;
    float x, z;                   // posizione (sim x, sim y)
    float alt;                    // quota dell'origine modello
    float dx, dz;                 // direzione di volo (unitaria, piano sim)
    float ex, ez;                 // punto d'uscita
    float cable_s;                // scala Y corrente del cavo
    float rot;                    // angolo rotori
    int carrying;
} gHeli;
static void shell_do_act(AppAction act);          // definita più sotto
static App gApp;                                  // stato shell (sezione GAME SHELL)

// --- economia biomassa v2 (BIOMASS_DESIGN): valuta UNICA, spesa a domanda.
// Niente convertitore, niente store per item: i kill riempiono il serbatoio, le
// azioni lo prosciugano al loro costo. Stato host: il modulo bio + le tre
// MODALITÀ-VERBO della barra (§4), esclusive fra loro. Reset in build_world.
static Bio  gBio;
static int  gAimRepair = 0;               // RIPARA (R): LMB tenuto = flusso bio->HP
static int  gRepairHold = 0;              // LMB giù nella modalità RIPARA
static int  gAdjOn = 0;                   // REGOLA (V): click su torretta = riorienta
static int  gAdjTid = -1;                 // torretta col cono in mano (drag in corso)
static float gAdjFacing = 0.0f;
static float gBioFlash = 0.0f;            // > 0: serbatoio pieno, kill SPRECATO
// LOOP_DESIGN D: resa scalata per i kill da mortaio (mortar.bio_yield). Vale
// SOLO dentro la host_blast del colpo (settato prima, ripristinato subito
// dopo: def_blast è sincrono). I kill da caduta dei lanciati arrivano step
// dopo e pagano resa piena (leak minore, annotato). Mine/RMB: resa piena.
static float gBioYieldMul = 1.0f;
// --- soldato giocabile (SOLDIER_DESIGN, LOOP_DESIGN F) --------------------
// Corpo = draggable nel core (invisibile a torrette/goal/missione, shovato
// dal PBD); modalità ESCLUSIVA coi verbi: dentro WASD muove, mouse mira,
// LMB mitra, RMB granata (bio). INVARIANTE: il corpo esiste solo in modalità
// (uscire = rientro alla base); il tick nel main loop fa rispettare la regola.
static Soldier *gSol = NULL;             // creato in build_world (def da balance)
static int   gSolMode = 0;               // modalità soldato (F / card SOLDATO)
static int   gSolFire = 0;               // LMB tenuto = mitra
static float gSolAimX = 0, gSolAimY = 0; // punto di mira (mondo)
static float gSolLastX = 0, gSolLastY = 0; // ultima pos nota (FX al down)
static int   gSolWant = 0;               // toggle richiesto (F/card), servito nel
                                         // frame body (dove s è in scope)
// granata (RMB): FICTION in volo come il colpo di mortaio, poi host_blast coi
// numeri di balance. Una sola in aria (il fuse è il rate limiter).
#define GREN_APEX 2.2f
static struct { int on; float t, ttot, ox, oy, x, y; } gGren;
// barra HP world-space del soldato (transitoria come le barre di reload:
// esiste solo in modalità — le barre permanenti restano bandite)
static float gSolBarX, gSolBarY, gSolBarF; static int gSolBarOn = 0;
// render skinned (model.h): assets/models/soldier.glb caricato al boot; se
// manca resta la sagoma verde di build_drag_mesh. Auto-scala dalla bbox bind
// pose a VAT_HORDE_SOL_H (default 1.80 m), piedi ancorati a bbox_min.y,
// centrato in XZ. VAT_HORDE_SOL_YAW (gradi) corregge il forward del modello
// (convenzione glTF: +Z verso camera). Clip: idle/run cercate per nome.
static Model     gSolMdl;
static AnimState gSolAnim;
static int       gSolMdlOk = 0, gSolClipIdle = -1, gSolClipRun = -1;
// gambe direzionali (twin-stick): clip scelta dall'angolo movimento-mira su
// 8 settori, il busto resta sulla mira. Fallback = run avanti (glb vecchi).
static int       gSolClipBack = -1, gSolClipLeft = -1, gSolClipRight = -1;
static int       gSolClipFL = -1, gSolClipFR = -1;
static int       gSolClipBL = -1, gSolClipBR = -1;
// da fermo: sparo col fucile spianato; lancio granata = GESTO CON LOCK
// (playtest 18/7: granata potente -> il costo è l'immobilità). La clip Mixamo
// (3.23 s: cintura, sicura coi denti, lancio) parte da SOL_TOSS_START (salta
// il rituale, tiene caricamento+lancio+recupero), il soldato resta FERMO per
// SOL_TOSS_WINDOW e la granata parte AL RILASCIO del braccio
// (SOL_TOSS_RELEASE dall'inizio finestra; picco velocità mano misurato a
// t=1.8 della clip). Morso/recall a metà gesto = lancio annullato, bio resa.
static int       gSolClipFire = -1, gSolClipToss = -1;
#define SOL_TOSS_START   1.15f   // offset nella clip (s)
#define SOL_TOSS_WINDOW  1.45f   // lock del soldato (s)
#define SOL_TOSS_RELEASE 0.65f   // spawn granata, dall'inizio finestra (s)
// SCAVALCAMENTO (SOLDIER_DESIGN): spingere contro una cella-struttura (muri
// del giocatore/scena, barricate — palazzi/prop/torrette esclusi) per
// SOL_CLIMB_PUSH s la scavalca. Il modulo soldier toglie il corpo e fa
// scivolare la posizione riportata fino all'atterraggio; qui la FICTION:
// clip "climb" ancorata al punto di partenza (root motion tenuta nel bake:
// è LEI che porta la mesh su e oltre il muro), poi "jump_down" ancorata in
// cima con scivolata dell'ancora a terra durante la caduta (il drop della
// clip è 0.90 m contro i ~2 m del muro: la differenza la assorbe l'ancora).
// Le costanti *_FWD/_UP/_HIP sono le misure model-space stampate da
// gfx/soldier_glb_make.py (moltiplicate a runtime per gSolMdlScale).
static int       gSolClipClimb = -1, gSolClipJump = -1;
static struct {
    int   on;                 // fiction attiva (il gioco è soldier_climbing)
    float t;                  // orologio fiction, avanzato dal TICK
    float x0, y0, yaw;        // ancora di partenza + direzione verso il muro
    float ex, ey;             // punto d'atterraggio fisico
} gSolClimb;
static float     gSolPushT = 0.0f;  // s spesi a spingere contro un muro scavalcabile
static float     gSolAimT  = 0.0f;  // coda "guarda la mira" dopo l'ultimo colpo
#define SOL_AIM_LINGER   0.35f   // s: evita il flip-flop di heading col tap-fire
// MITRA IN MANO (2026-07-19): fucile procedurale flat-color (4 box, canna
// lungo +X locale) agganciato alla posizione WORLD del bone RightHand
// (anim_bone_global x model matrix) e orientato ORIZZONTALE sull'heading di
// render — niente assi del bone: a questa scala la canna livellata legge
// meglio e la bocca resta prevedibile per tracer e vampa. Verts trasformati
// su CPU ogni frame (progFlat, layout 9-float come il buffer strutture).
// La bocca dell'ULTIMO frame disegnato (gGunMuz, world x/quota/z) è
// l'origine di tracer+muzzle flash del tick successivo (1 frame di ritardo,
// invisibile). Durante il TOSS il fucile passa alla mano SINISTRA (la destra
// lancia la granata, la sinistra lo regge); nascosto solo in climb.
static int    gSolHandBone = -1, gSolHandBoneL = -1;
static float *gGunVerts = NULL, *gGunXf = NULL; static int gGunNV = 0;
static GLuint gGunVao = 0, gGunVbo = 0;
static float  gGunMuz[3]; static int gGunMuzOn = 0;
static int    gGunFlash = 0; static float gGunFlashAng = 0.0f; // vampa pendente (dal tick)
#define GUN_MUZ_X 0.60f          // punta della canna dal grip (m)
static void gun_box(float *v, int *n, float x0, float x1, float y0, float y1,
                    float z0, float z1, float r, float g, float b){
    static const int F[6][4][3]={ // 6 facce come tri-strip srotolata (a,b,c / a,c,d)
        {{1,0,0},{1,1,0},{1,1,1},{1,0,1}},   // +X
        {{0,0,1},{0,1,1},{0,1,0},{0,0,0}},   // -X
        {{0,1,0},{0,1,1},{1,1,1},{1,1,0}},   // +Y
        {{0,0,0},{1,0,0},{1,0,1},{0,0,1}},   // -Y
        {{0,0,1},{1,0,1},{1,1,1},{0,1,1}},   // +Z
        {{0,0,0},{0,1,0},{1,1,0},{1,0,0}},   // -Z
    };
    static const float N[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    const float X[2]={x0,x1}, Y[2]={y0,y1}, Z[2]={z0,z1};
    for(int f=0;f<6;f++) for(int t=0;t<6;t++){
        int k = t<3 ? t : (t==3?0:t-2);      // 0,1,2, 0,2,3
        const int *c=F[f][k]; float *o=v+(*n)*9; (*n)++;
        o[0]=X[c[0]]; o[1]=Y[c[1]]; o[2]=Z[c[2]];
        o[3]=N[f][0]; o[4]=N[f][1]; o[5]=N[f][2];
        o[6]=r; o[7]=g; o[8]=b;
    }
}
static void build_gun_mesh(void){
    gGunVerts=(float*)malloc(4*36*9*sizeof(float)); int n=0;
    gun_box(gGunVerts,&n,-0.30f,-0.10f,-0.055f,0.015f,-0.018f,0.018f,0.16f,0.13f,0.10f); // calcio
    gun_box(gGunVerts,&n,-0.12f, 0.28f,-0.030f,0.038f,-0.022f,0.022f,0.10f,0.10f,0.11f); // castello
    gun_box(gGunVerts,&n, 0.28f,GUN_MUZ_X,-0.012f,0.014f,-0.012f,0.012f,0.07f,0.07f,0.08f); // canna
    gun_box(gGunVerts,&n, 0.00f, 0.07f,-0.130f,-0.030f,-0.015f,0.015f,0.09f,0.09f,0.10f); // caricatore
    gGunNV=n;
    gGunXf=(float*)malloc((size_t)n*9*sizeof(float));
    glGenVertexArrays(1,&gGunVao); glBindVertexArray(gGunVao);
    glGenBuffers(1,&gGunVbo); glBindBuffer(GL_ARRAY_BUFFER,gGunVbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)n*9*sizeof(float),NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,9*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,0,9*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,9*sizeof(float),(void*)(6*sizeof(float)));glEnableVertexAttribArray(2);
    glBindVertexArray(0);
}
#define SOL_CLIMB_PUSH   0.25f   // pressione richiesta per agganciare (s)
#define SOL_CLIMB_FWD    1.40f   // avanzamento della climb trimmata (model m)
#define SOL_CLIMB_ENDHIP 1.94f   // quota hips a fine climb (model m)
#define SOL_JUMP_FWD     2.28f   // avanzamento del jump_down (model m)
#define SOL_JUMP_HIP0    0.85f   // quota hips a inizio jump_down (model m)
#define SOL_JUMP_T0      0.73f   // inizio caduta nella clip jump_down (s)
#define SOL_JUMP_T1      1.47f   // atterraggio nella clip jump_down (s)
static float     gSolTossT  = 0.0f;
static struct { int on; float x, y; } gGrenPend;   // pagata, in attesa del gesto
static int       gSolFiring = 0;    // trigger valido nell'ultimo tick
static GLuint    gSkinProg = 0;
static float     gSolMdlScale = 1.0f, gSolMdlOff[3];
static float     gSolYaw = 0.0f, gSolYawAdj = 0.0f;
static int sol_pick_clip(const Model *m, const char **names, int n){
    for(int i=0;i<n;i++){ int c=model_find_clip(m,names[i]); if(c>=0) return c; }
    return -1;
}
// model matrix del soldato: T(mondo) * R_y(yaw) * S(scale) * T(ancora)
static void sol_model_mat(mat4 m, float wx, float wy, float yaw){
    float zb=ter_z(wx,wy), sc=gSolMdlScale, cy=cosf(yaw), sy=sinf(yaw);
    m_identity(m);
    m[0]=cy*sc;  m[2]=-sy*sc;
    m[5]=sc;
    m[8]=sy*sc;  m[10]=cy*sc;
    float ox=gSolMdlOff[0]*sc, oy=gSolMdlOff[1]*sc, oz=gSolMdlOff[2]*sc;
    m[12]=wx + cy*ox + sy*oz;
    m[13]=zb + oy;
    m[14]=wy - sy*ox + cy*oz;
}
// deploy: primo punto LIBERO (agenti+muri) su anelli attorno alla base — il
// container LZ è solido, dentro non si spawna
static int soldier_spot(SimP *s, float bx, float by, float r,
                        float *ox, float *oy){
    for(float rr=2.4f; rr<=10.0f; rr+=0.8f)
        for(int k=0;k<16;k++){
            float a=(float)k*(6.2831853f/16.0f);
            float x=bx+rr*cosf(a), y=by+rr*sinf(a);
            if(simp_free_at(s,x,y,r)){ *ox=x; *oy=y; return 1; }
        }
    return 0;
}
// costi delle azioni (§6): manopole balance.cfg (mortar.cost, bio.*,
// turret.*.reload_cost), resa per body da bio.yield.*
#define BIO_MORTAR_COST  (gBal.mortar.cost)      // un colpo di mortaio
#define BIO_REPAIR_RATE  (gBal.bio.repair_rate)  // HP/s a mantenimento (1 bio = 1 HP)
#define BIO_ADJUST_COST  (gBal.bio.adjust_cost)  // REGOLA: gratis in v1
#define BIO_RELOAD_COST(k) (gBal.tur[k].reload_cost)   // per kind
// colpo del mitra (SoldierShotFn): tracer dal petto al punto d'impatto, danno
// via def_damage_agent (stessa pipeline delle torrette: HP/wound/gib/bounty).
// La resa bio dei kill è scalata da soldier.bio_yield col pattern del mortaio.
typedef struct { DefGame *g; SimP *s; } SolShotCtx;
static void on_soldier_shot(void *ud, float ox, float oy,
                            float ex, float ey, int hit, float dmg){
    SolShotCtx *c = (SolShotCtx *)ud;
    // tracer dalla BOCCA del mitra (posizione dell'ultimo frame disegnato);
    // fallback petto se il fucile non è ancora stato disegnato. La vampa la
    // emette il main dopo soldier_step (fx è un local del main): qui si arma.
    float o0=ox, o1=ter_z(ox,oy)+1.25f, o2=oy;
    if(gGunMuzOn){ o0=gGunMuz[0]; o1=gGunMuz[1]; o2=gGunMuz[2]; }
    gGunFlash=1; gGunFlashAng=atan2f(ey-o2, ex-o0);
    tracer_spawn(o0, o1, o2,
                 ex, ter_z(ex,ey)+(hit>=0?0.95f:0.25f), ey, 0);
    if(hit >= 0){
        SimPHandle h = simp_handle_of(c->s, hit);   // indice denso: convertire SUBITO
        float mul = gBioYieldMul;
        gBioYieldMul = gBal.soldier.bio_yield;
        def_damage_agent(c->g, h, dmg);
        gBioYieldMul = mul;
    }
}

static void host_bio_kill(DefBody body){
    if (gApp.state != APP_ASSAULT) return;    // biomassa = valuta dell'ASSALTO
    float lost = bio_add(&gBio, gBioYieldMul *
                         gBal.bio.yield[((int)body >= 0 && body < BT_COUNT) ? body : BT_MAN]);
    if (lost > 0.0f) gBioFlash = 0.5f;        // §3: lo spreco lampeggia, non chiede nulla
}
#ifdef GAME_SHELL
// una sola modalità-verbo alla volta (§4): accenderne una spegne le altre.
// 2026-07-19: i verbi NON recallano più il soldato — col soldato in campo il
// corpo RESTA dov'è (lure e morsi attivi: il costo di usare il mortaio è
// l'immobilità e il rischio), il mouse passa al verbo (il blocco eventi
// gAimMort/gAimRepair viene prima e consuma i click) e all'uscita dal verbo
// il controllo torna da solo (gSolMode resta 1). Senza soldato in campo,
// semantica di prima.
static void bio_mode_set(int mort, int rep, int adj){
    gAimMort = mort; gAimRepair = rep; gAdjOn = adj;
    if (!rep) gRepairHold = 0;
    if (!adj) gAdjTid = -1;
    gSolFire = 0;
    if (!(gSol && soldier_active(gSol))) gSolMode = 0;
}
// LOOP_DESIGN C: in ASSALTO si costruisce pagando BIOMASSA a prezzo maggiorato
// (ceil(markup × costo $), bio.build_markup; budget e bio NON convertibili).
// Il wallet viene agganciato a plc solo in APP_ASSAULT (per-frame nel main
// loop); in PREP resta il budget legacy. Catalogo d'assalto = sottoinsieme
// "da campo" a sblocco progressivo (decisione 2): per ora light + barricata.
static int  bio_wallet_price(void *u, int cost){ (void)u;
    return (int)ceilf((float)cost * gBal.bio.build_markup); }
static int  bio_wallet_avail(void *u){ (void)u; return (int)bio_tank(&gBio); }
static void bio_wallet_spend(void *u, int amount){ (void)u;
    bio_take(&gBio, (float)amount); }
static const PlWallet gBioWallet =
    { bio_wallet_price, bio_wallet_avail, bio_wallet_spend, 0 };
static const int BUILD_CAT[] = { 0 /*Leggera*/, 4 /*Barricata*/ };
#define BUILD_NCAT ((int)(sizeof(BUILD_CAT)/sizeof(BUILD_CAT[0])))
// LOOP_DESIGN F+C (2026-07-18): col soldato IN CAMPO la costruzione d'assalto
// è SUA — la card non lo recalla, il ghost è ancorato un passo davanti a lui
// (verso la mira: piazzarla sotto i piedi lo murerebbe dentro), LMB piazza,
// facing torretta = la sua mira. REGOLA in assalto col soldato in campo vale
// solo a portata di braccio (è lui che la gira). Senza soldato in campo resta
// il piazzamento a cursore di LOOP_DESIGN C.
#define SOL_BUILD_AHEAD  1.2f   // m davanti al soldato
#define SOL_ADJUST_REACH 3.5f   // m: oltre, il soldato deve avvicinarsi
static int sol_builder(void){
    return gSolMode && gSol && soldier_active(gSol);
}
static void sol_build_spot(float *bx, float *by){
    float sx=soldier_x(gSol), sy=soldier_y(gSol);
    float dx=gSolAimX-sx, dy=gSolAimY-sy, d=sqrtf(dx*dx+dy*dy);
    if(d<1e-3f){ dx=sinf(gSolYaw); dy=cosf(gSolYaw); d=1.0f; }
    *bx=sx+dx*(SOL_BUILD_AHEAD/d); *by=sy+dy*(SOL_BUILD_AHEAD/d);
}
// spegne gli altri verbi ma NON il soldato (variante di bio_mode_set per le
// modalità che il soldato in campo possiede: COSTRUISCI e REGOLA)
static void sol_verbs_off(void){
    gAimMort=0; gAimRepair=0; gRepairHold=0; gAdjOn=0; gAdjTid=-1; gSolFire=0;
}
#endif

// quota a cui la BASE del container appeso tocca terra alla lz
static float heli_alt_low(void){
    return ter_z(gLzX,gLzY) + BASE_H + gHeliM.cable_len*HELI_CABLE_FLY - gHeliM.cable_top;
}
static void heli_begin(int mode, const Scene *sc){
    float a=gLzYaw;
    gHeli.dx=cosf(a); gHeli.dz=sinf(a);
    float R=0.5f*sqrtf(sc->world_w*sc->world_w+sc->world_h*sc->world_h)+40.0f;
    gHeli.x=gLzX-gHeli.dx*R; gHeli.z=gLzY-gHeli.dz*R;
    gHeli.ex=gLzX+gHeli.dx*R; gHeli.ez=gLzY+gHeli.dz*R;
    gHeli.alt=ter_z(gLzX,gLzY)+HELI_CRUISE;
    gHeli.phase=HELI_IN; gHeli.t=0.0f; gHeli.rot=0.0f;
    gHeli.active=mode;
    if(mode==1){ gHeli.carrying=1; gHeli.cable_s=HELI_CABLE_FLY; gLzHeld=1; }
    else       { gHeli.carrying=0; gHeli.cable_s=HELI_CABLE_MIN; gLzHeld=0; }
}
static void heli_update(float dt){
    if(!gHeli.active) return;
    // stato shell cambiato sotto i piedi (skip con INVIO, ESC al menu):
    // finalizza — container a terra, elicottero via.
    if((gHeli.active==1 && gApp.state!=APP_DEPLOY) ||
       (gHeli.active==2 && gApp.state!=APP_EXTRACT)){
        gHeli.active=0; gLzHeld=0; return; }
    gHeli.rot+=HELI_ROTOR_W*dt;
    float low=heli_alt_low();
    switch(gHeli.phase){
    case HELI_IN:{
        float rx=gLzX-gHeli.x, rz=gLzY-gHeli.z, d=sqrtf(rx*rx+rz*rz), st=HELI_SPEED*dt;
        if(d<=st){ gHeli.x=gLzX; gHeli.z=gLzY; gHeli.phase=HELI_DOWN; }
        else { gHeli.x+=rx/d*st; gHeli.z+=rz/d*st; }
    } break;
    case HELI_DOWN:
        gHeli.alt-=HELI_VSPEED*dt;
        if(gHeli.alt<=low){ gHeli.alt=low; gHeli.phase=HELI_ACT; gHeli.t=0.0f; }
        break;
    case HELI_ACT:
        gHeli.t+=dt;
        if(gHeli.active==1){                   // consegna: posa, poi ritira il cavo
            if(gHeli.carrying && gHeli.t>0.6f){ gHeli.carrying=0; gLzHeld=0; }
            if(!gHeli.carrying){
                gHeli.cable_s-=(HELI_CABLE_FLY-HELI_CABLE_MIN)/0.9f*dt;
                if(gHeli.cable_s<=HELI_CABLE_MIN){ gHeli.cable_s=HELI_CABLE_MIN; gHeli.phase=HELI_UP; }
            }
        } else {                               // estrazione: srotola, aggancia, solleva
            if(gHeli.cable_s<HELI_CABLE_FLY){
                gHeli.cable_s+=(HELI_CABLE_FLY-HELI_CABLE_MIN)/0.9f*dt;
                if(gHeli.cable_s>=HELI_CABLE_FLY){ gHeli.cable_s=HELI_CABLE_FLY; gHeli.t=0.0f; }
            } else if(!gHeli.carrying && gHeli.t>0.5f){ gHeli.carrying=1; gLzHeld=1; gHeli.phase=HELI_UP; }
        }
        break;
    case HELI_UP:
        gHeli.alt+=HELI_VSPEED*1.3f*dt;
        if(gHeli.alt>=ter_z(gLzX,gLzY)+HELI_CRUISE){ gHeli.phase=HELI_OUT; }
        break;
    case HELI_OUT:{
        float rx=gHeli.ex-gHeli.x, rz=gHeli.ez-gHeli.z, d=sqrtf(rx*rx+rz*rz), st=HELI_SPEED*dt;
        if(d<=st){ gHeli.active=0; gLzHeld=0;
            shell_do_act(app_input(&gApp,APP_IN_CONFIRM));   // fine naturale = skip
        } else { gHeli.x+=rx/d*st; gHeli.z+=rz/d*st; }
    } break;
    }
}
// matrici: M_part = T(pos)·RotY(yaw) · T(piv)·R(axis,ang)·S(1,sy,1)·T(-piv)
static void heli_place_mat(mat4 m){
    float yaw=atan2f(gHeli.dx,gHeli.dz), c=cosf(yaw), s=sinf(yaw);
    m_identity(m);
    m[0]=c;  m[2]=-s;
    m[8]=s;  m[10]=c;
    m[12]=gHeli.x; m[13]=gHeli.alt; m[14]=gHeli.z;
}
static void heli_part_mat(mat4 out, const mat4 place, const HeliPart *p,
                          float ang, float sy){
    mat4 L; float c=cosf(ang), s=sinf(ang), C=1.0f-c;
    float ax=p->axis[0], ay=p->axis[1], az=p->axis[2];
    float R[9]={ ax*ax*C+c,    ax*ay*C-az*s, ax*az*C+ay*s,
                 ay*ax*C+az*s, ay*ay*C+c,    ay*az*C-ax*s,
                 az*ax*C-ay*s, az*ay*C+ax*s, az*az*C+c };
    // colonne = R·S; traslazione = piv - R·S·piv
    L[0]=R[0];    L[1]=R[3];    L[2]=R[6];    L[3]=0;
    L[4]=R[1]*sy; L[5]=R[4]*sy; L[6]=R[7]*sy; L[7]=0;
    L[8]=R[2];    L[9]=R[5];    L[10]=R[8];   L[11]=0;
    float px=p->piv[0], py=p->piv[1], pz=p->piv[2];
    L[12]=px-(L[0]*px+L[4]*py+L[8]*pz);
    L[13]=py-(L[1]*px+L[5]*py+L[9]*pz);
    L[14]=pz-(L[2]*px+L[6]*py+L[10]*pz);
    L[15]=1;
    m_mul(out,place,L);
}
static void heli_draw(GLuint prog, GLint uVP_, GLint uModel_, const mat4 vp){
    if(!gHeli.active || !gHeliM.ok) return;
    glUseProgram(prog); glUniformMatrix4fv(uVP_,1,GL_FALSE,vp);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(prog,"uTex"),0);
    mat4 place, mm; heli_place_mat(place);
    glBindTexture(GL_TEXTURE_2D,gHeliM.tex);
    glUniformMatrix4fv(uModel_,1,GL_FALSE,place);
    glBindVertexArray(gHeliM.body.vao);
    glDrawElements(GL_TRIANGLES,gHeliM.body.ni,GL_UNSIGNED_SHORT,0);
    // rotori: 2 copie sfasate di 90° per pala — il "trucco" anti-strobo: a
    // spin pieno le copie si fondono in un disco pieno di pale.
    for(int cpy=0;cpy<2;cpy++){
        float off=cpy*1.5707963f;
        heli_part_mat(mm,place,&gHeliM.r1,gHeli.rot+off,1.0f);
        glUniformMatrix4fv(uModel_,1,GL_FALSE,mm);
        glBindVertexArray(gHeliM.r1.vao);
        glDrawElements(GL_TRIANGLES,gHeliM.r1.ni,GL_UNSIGNED_SHORT,0);
        heli_part_mat(mm,place,&gHeliM.r2,-gHeli.rot+off,1.0f);   // controrotante
        glUniformMatrix4fv(uModel_,1,GL_FALSE,mm);
        glBindVertexArray(gHeliM.r2.vao);
        glDrawElements(GL_TRIANGLES,gHeliM.r2.ni,GL_UNSIGNED_SHORT,0);
    }
    glBindTexture(GL_TEXTURE_2D,gHeliM.greyTex);
    heli_part_mat(mm,place,&gHeliM.cable,0.0f,gHeli.cable_s);
    glUniformMatrix4fv(uModel_,1,GL_FALSE,mm);
    glBindVertexArray(gHeliM.cable.vao);
    glDrawElements(GL_TRIANGLES,gHeliM.cable.ni,GL_UNSIGNED_SHORT,0);
    glBindVertexArray(0);
}
#endif /* GAME_SHELL */
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
// Sweep sul bbox mantenuto da defense (def_struct_bbox): include le barricate
// piazzate a runtime (PLACEMENT_DESIGN.md) e resta stretto sulle mappe grandi
// (lo sweep dell'intera griglia costava ms sulle mappe L). maxV protegge il VBO.
#define WALL_TH 0.20f   // spessore VISIVO dei muri (la cella nav resta 0.5 m)
static int build_struct_mesh(DefGame *g, float cell, float *buf, int maxV){
    int bx0,by0,bx1,by1;
    if(def_struct_count(g)==0 || !def_struct_bbox(g,&bx0,&by0,&bx1,&by1)) return 0;
    int c=0; float H=1.95f;   // altezza render ~= top-out della clip climb
                              // (hips a 1.94 m mondo a fine salita)
    for(int cy=by0; cy<=by1; cy++)
    for(int cx=bx0; cx<=bx1; cx++){
        if(c+30 > maxV) return c;                         // cap: non sforare il VBO
        int id=def_cell_struct(g,cx,cy); if(id<0) continue;
        if(def_struct_is_turret(g,id)) continue;          // drawn as a turret pillar
        if(id==gLzCore) continue;                         // il container si disegna come box unico (sotto)
        if(id<PROP_WORLD_MAX_STRUCT && gPropW.struct_is_prop[id]) continue; // il prop disegna la sua mesh (§6)
#ifdef GAME_SHELL
        if(id<PLMOD_SIDCAP && gSidMod[id]) continue;      // modulo linea: box ruotato (sotto)
#endif
        float frac=def_struct_hp(g,id)/ (def_struct_hp_max(g,id)+1e-3f); // 1..0
        float t=0.35f+0.65f*frac;                         // darken with damage
        float cr,cg,cb;
        if(id==gCoreId){ cr=0.75f*t; cg=0.16f*t; cb=0.16f*t; }   // core = red
        else           { cr=0.55f*t; cg=0.57f*t; cb=0.62f*t; }   // ring = steel
        float x0=cx*cell, x1=x0+cell, z0=cy*cell, z1=z0+cell;
        // muri sottili a vista: le corse spesse UNA cella diventano lastre da
        // WALL_TH lungo la direzione dei vicini struttura; celle con vicini in
        // ENTRAMBE le direzioni (angoli, interni di muri spessi tipo i
        // bastioni 3-celle di arena4) restano box pieni — all'angolo di un
        // muro sottile il box fa da pilastro, i bastioni restano massicci.
        // La cella isolata e' un pilastrino WALL_TH x WALL_TH.
        float mx=(x0+x1)*0.5f, mz=(z0+z1)*0.5f, hth=0.5f*WALL_TH;
        int runx = def_cell_struct(g,cx+1,cy)>=0 || def_cell_struct(g,cx-1,cy)>=0;
        int runz = def_cell_struct(g,cx,cy+1)>=0 || def_cell_struct(g,cx,cy-1)>=0;
        float zb=ter_z(mx,mz), H1=zb+H;                   // seat on terrain
#define VS(PX,PY,PZ,NX,NY,NZ) do{float*o=buf+c*9;o[0]=PX;o[1]=PY;o[2]=PZ;\
        o[3]=NX;o[4]=NY;o[5]=NZ;o[6]=cr;o[7]=cg;o[8]=cb;c++;}while(0)
#define QS(ax,ay,az,bx,by,bz,px2,py2,pz2,dx,dy,dz,nx,ny,nz) do{ \
        VS(ax,ay,az,nx,ny,nz);VS(bx,by,bz,nx,ny,nz);VS(px2,py2,pz2,nx,ny,nz); \
        VS(ax,ay,az,nx,ny,nz);VS(px2,py2,pz2,nx,ny,nz);VS(dx,dy,dz,nx,ny,nz);}while(0)
#define BX(X0,X1,Z0,Z1) do{ \
        QS(X0,H1,Z0, X1,H1,Z0, X1,H1,Z1, X0,H1,Z1, 0,1,0);    /* top */ \
        QS(X1,zb,Z0, X1,zb,Z1, X1,H1,Z1, X1,H1,Z0, 1,0,0);    /* +X  */ \
        QS(X0,zb,Z1, X0,zb,Z0, X0,H1,Z0, X0,H1,Z1, -1,0,0);   /* -X  */ \
        QS(X0,zb,Z1, X1,zb,Z1, X1,H1,Z1, X0,H1,Z1, 0,0,1);    /* +Z  */ \
        QS(X1,zb,Z0, X0,zb,Z0, X0,H1,Z0, X1,H1,Z0, 0,0,-1);}while(0)
        if(runx&&runz)    BX(x0,x1, z0,z1);
        else if(runx)     BX(x0,x1, mz-hth,mz+hth);
        else if(runz)     BX(mx-hth,mx+hth, z0,z1);
        else              BX(mx-hth,mx+hth, mz-hth,mz+hth);
#undef VS
#undef QS
#undef BX
    }
#ifdef GAME_SHELL
    // moduli di linea (PREP_UI §5): box ruotati lungo la linea al posto delle
    // celle saltate sopra. Stesso acciaio + scurimento col danno; il crollo li
    // fa sparire come le celle (le celle fisiche le libera defense).
    for(int m=0;m<gPlModN;m++){
        const PlModule *md=&gPlMod[m];
        if(c+30>maxV) return c;
        if(def_struct_collapsed(g,md->sid)) continue;
        float frac=def_struct_hp(g,md->sid)/(def_struct_hp_max(g,md->sid)+1e-3f);
        float t=0.35f+0.65f*frac;
        float cr=0.55f*t, cg=0.57f*t, cb=0.62f*t;
        float ca=cosf(md->ang), sa=sinf(md->ang);
        float hl=0.5f*md->len, ht=0.5f*md->th;
        /* corners: center ± hl·u ± ht·v, u=(ca,sa) lungo linea, v=(-sa,ca) */
        float px[4]={ md->x-ca*hl+sa*ht, md->x+ca*hl+sa*ht,
                      md->x+ca*hl-sa*ht, md->x-ca*hl-sa*ht };
        float pz[4]={ md->y-sa*hl-ca*ht, md->y+sa*hl-ca*ht,
                      md->y+sa*hl+ca*ht, md->y-sa*hl+ca*ht };
        float zb=ter_z(md->x,md->y), H1=zb+H;
#define VM(PX,PY,PZ,NX,NY,NZ) do{float*o=buf+c*9;o[0]=PX;o[1]=PY;o[2]=PZ;\
        o[3]=NX;o[4]=NY;o[5]=NZ;o[6]=cr;o[7]=cg;o[8]=cb;c++;}while(0)
#define QM(A,B,YA,YB,nx,nz) do{ /* lato A->B, sotto YA sopra YB */ \
        VM(px[A],YA,pz[A],nx,0,nz);VM(px[B],YA,pz[B],nx,0,nz);\
        VM(px[B],YB,pz[B],nx,0,nz);VM(px[A],YA,pz[A],nx,0,nz);\
        VM(px[B],YB,pz[B],nx,0,nz);VM(px[A],YB,pz[A],nx,0,nz);}while(0)
        VM(px[0],H1,pz[0],0,1,0);VM(px[1],H1,pz[1],0,1,0);VM(px[2],H1,pz[2],0,1,0);
        VM(px[0],H1,pz[0],0,1,0);VM(px[2],H1,pz[2],0,1,0);VM(px[3],H1,pz[3],0,1,0);
        QM(1,2,zb,H1,  ca, sa);      /* +u (testa) */
        QM(3,0,zb,H1, -ca,-sa);      /* -u (coda)  */
        QM(2,3,zb,H1, -sa, ca);      /* +v (fianco) */
        QM(0,1,zb,H1,  sa,-ca);      /* -v (fianco) */
#undef VM
#undef QM
    }
#endif
    // BASE container (fase 1b/1c): il core della LZ è il modello
    // base_and_mortar (container + mortaio che mira, BASE_DESIGN §3) o, senza
    // glb, UN box orientato 6,1×2,44×2,6 m al posto dei cubetti per-cella
    // saltati sopra. Si scurisce col danno e sparisce al crollo (celle
    // liberate da defense). Mentre è appeso all'elicottero (gLzHeld) non si
    // disegna a terra: lo stampa il blocco cinematica in fondo.
    if(gLzCore>=0 && !def_struct_collapsed(g,gLzCore) && !gLzHeld && c+30<=maxV){
        float frac=def_struct_hp(g,gLzCore)/(def_struct_hp_max(g,gLzCore)+1e-3f);
        float t=0.35f+0.65f*frac;
        if(gBaseM.ok){
            c=base_model_emit(buf,c,maxV,t, gLzX,ter_z(gLzX,gLzY),gLzY, gLzYaw,
                              gMortAz-gLzYaw, gMortTilt);
            return c;
        }
        float cr=0.82f*t, cg=0.40f*t, cb=0.12f*t;        // arancio container da spedizione
        float ca=cosf(gLzYaw), sa=sinf(gLzYaw);
        float hw=0.5f*BASE_W, ht=0.5f*BASE_D;            // half-length lungo u, half-depth lungo v
        /* corners: center ± hw·u ± ht·v, u=(ca,sa) lunghezza, v=(-sa,ca) profondità */
        float px[4]={ gLzX-ca*hw+sa*ht, gLzX+ca*hw+sa*ht,
                      gLzX+ca*hw-sa*ht, gLzX-ca*hw-sa*ht };
        float pz[4]={ gLzY-sa*hw-ca*ht, gLzY+sa*hw-ca*ht,
                      gLzY+sa*hw+ca*ht, gLzY-sa*hw+ca*ht };
        float zb=ter_z(gLzX,gLzY), H1=zb+BASE_H;
#define VC(PX,PY,PZ,NX,NY,NZ) do{float*o=buf+c*9;o[0]=PX;o[1]=PY;o[2]=PZ;\
        o[3]=NX;o[4]=NY;o[5]=NZ;o[6]=cr;o[7]=cg;o[8]=cb;c++;}while(0)
#define QC(A,B,YA,YB,nx,nz) do{ /* lato A->B, sotto YA sopra YB */ \
        VC(px[A],YA,pz[A],nx,0,nz);VC(px[B],YA,pz[B],nx,0,nz);\
        VC(px[B],YB,pz[B],nx,0,nz);VC(px[A],YA,pz[A],nx,0,nz);\
        VC(px[B],YB,pz[B],nx,0,nz);VC(px[A],YB,pz[A],nx,0,nz);}while(0)
        VC(px[0],H1,pz[0],0,1,0);VC(px[1],H1,pz[1],0,1,0);VC(px[2],H1,pz[2],0,1,0);
        VC(px[0],H1,pz[0],0,1,0);VC(px[2],H1,pz[2],0,1,0);VC(px[3],H1,pz[3],0,1,0);
        QC(1,2,zb,H1,  ca, sa);      /* +u (testa) */
        QC(3,0,zb,H1, -ca,-sa);      /* -u (coda)  */
        QC(2,3,zb,H1, -sa, ca);      /* +v (fianco) */
        QC(0,1,zb,H1,  sa,-ca);      /* -v (fianco) */
#undef VC
#undef QC
    }
#ifdef GAME_SHELL
    // container APPESO al cavo durante le cinematiche (BASE_DESIGN §4): stessa
    // mesh, quota = fondo del cavo (ancora - lunghezza·scala), già allo yaw
    // finale della lz (carico imbragato orientato).
    if(gLzHeld && gHeli.active && gBaseM.ok){
        float basey=gHeli.alt + gHeliM.cable_top - gHeliM.cable_len*gHeli.cable_s - BASE_H;
        c=base_model_emit(buf,c,maxV,1.0f, gHeli.x,basey,gHeli.z, gLzYaw,
                          gMortAz-gLzYaw, gMortTilt);
    }
#endif
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

// missione dichiarata nel .scn (GAME_PLAN fase A): la macchina possiede i
// director (uno per exit) e li fa emettere solo in ASSAULT. gMissionOn=0 =
// scena legacy -> director unico di demo come sempre.
static Mission gMission; static int gMissionOn=0;

// ondate scriptate (LOOP_DESIGN A): flash HUD del bonus "chiama la prossima"
static float gWaveMsgT = 0.0f;
#ifdef GAME_SHELL
static char  gWaveMsg[64];
#endif

// INVIO in assalto = "chiama la prossima": l'ondata annunciata parte SUBITO e
// i secondi di pausa risparmiati diventano biomassa (wave.bonus, alla Kingdom
// Rush). No-op se nessuna ondata e' in annuncio.
static void host_call_next_wave(void){
    if(!gMissionOn) return;
    float saved = mission_call_next(&gMission);
    if(saved <= 0.0f) return;
#ifdef GAME_SHELL
    int bonus = (int)(saved*gBal.wave.bonus_per_s + 0.5f);
    if(bonus > 0) bio_add(&gBio,(float)bonus);
    if(bonus > 0)
        snprintf(gWaveMsg,sizeof gWaveMsg,"ONDATA CHIAMATA: +%d BIOMASSA",bonus);
    else
        snprintf(gWaveMsg,sizeof gWaveMsg,"ONDATA CHIAMATA");
    gWaveMsgT = 2.5f;
    au_play(SND_MENU_SELECT);
#endif
    printf("ondata chiamata: %.1fs risparmiati\n",(double)saved);
}

#ifdef GAME_SHELL
// direzione bussola del centro-exit rispetto alla LZ (o al centro mondo),
// per il banner d'annuncio. Convenzione assi mondo: +x = EST, +y = NORD.
static const char *exit_compass(const Scene *sc,int ei){
    static const char *N8[8]={"EST","NORD-EST","NORD","NORD-OVEST",
                              "OVEST","SUD-OVEST","SUD","SUD-EST"};
    if(ei < 0 || ei >= sc->n_exit) return "?";
    const SceneExit *ex=&sc->exits[ei];
    float cx = sc->has_lz ? sc->lz_x : sc->world_w*0.5f;
    float cy = sc->has_lz ? sc->lz_y : sc->world_h*0.5f;
    float dx = ex->x+0.5f*ex->w-cx, dy = ex->y+0.5f*ex->h-cy;
    const float PI8 = 0.39269908f;                    /* pi/8 */
    int k = (int)floorf((atan2f(dy,dx)+PI8)/(2.0f*PI8));
    return N8[((k%8)+8)%8];
}

// riga del banner d'annuncio ("ONDATA 2/4 - 60 NEMICI (TANK!) DA OVEST E SUD
// - 12 S | INVIO: SUBITO (+BIO)"); 0 = nessuna ondata in annuncio.
static int wave_banner(const Scene *sc,char *out,size_t n){
    MissionWaveInfo wi;
    if(!gMissionOn || !mission_wave_pending(&gMission,&wi)) return 0;
    int tot=0, tank=0, obese=0;
    char dirs[64]=""; int nd=0;
    for(int k=0;k<wi.n;k++){
        tot += wi.e[k].count;
        if(wi.e[k].tank_pct  > 0) tank = 1;
        if(wi.e[k].obese_pct > 0) obese = 1;
        const char *d = exit_compass(sc,wi.e[k].exit_idx);
        if(!strstr(dirs,d)){                 // dedupe (due entry, stessa exit)
            if(nd++) strncat(dirs," E ",sizeof dirs-strlen(dirs)-1);
            strncat(dirs,d,sizeof dirs-strlen(dirs)-1);
        }
    }
    snprintf(out,n,"ONDATA %d/%d - %d NEMICI%s%s DA %s - %d S | INVIO: SUBITO (+BIO)",
             wi.index,wi.total,tot,tank?" +TANK":"",obese?" +OBESI":"",
             dirs,(int)(wi.countdown+0.5f));
    return 1;
}
#endif /* GAME_SHELL */

// entita' `lz x y` (fase A): goal centrale 3x3 (profondo >=2 celle, trappola
// tank di M3.5) + anello 5x5 di celle-core assediabili (is_core: crollo =
// sconfitta). L'elicottero e' fiction renderer-side (placeholder: pilastrino
// del core); HP da VAT_HORDE_LZ_HP (default 1500).
// La base è un CONTAINER (BASE_DESIGN §3): footprint reale di un container ISO
// 20" (~6,1 × 2,44 m, carico a gancio di un Chinook), orientabile via lz yaw.
// (gLzCore/gLzX/gLzY/gLzYaw + BASE_W/D/H sono dichiarati sopra build_struct_mesh.)
// Ogni cella del rettangolo ruotato diventa una cella della struttura assediabile
// (is_core), TRANNE la cella centrale che resta goal NON solido: il core §316 di
// sim_particles ammette solo goal non murati come sorgenti, quindi l'orda è
// attratta dal cuore sigillato e assedia il perimetro del container (come il
// vecchio anello 5×5, generalizzato al rettangolo). Sconfitta al crollo (def_lost).
typedef struct { DefGame *g; int sid, ccx, ccy; } LzFootCtx;
static void lz_foot_cb(void *ud, int cx, int cy){
    LzFootCtx *c=ud;
    if(cx==c->ccx && cy==c->ccy) return;      // cavità centrale: goal, non struttura
    def_struct_cell(c->g, c->sid, cx, cy);    // corpo del container: muro assediabile
}
static void build_lz_core(DefGame *g, SimP *s, const Scene *sc){
    gLzCore=-1;
    float cell=sc->cell; int gw=simp_grid_w(s), gh=simp_grid_h(s);
    float a=sc->lz_yaw*0.01745329252f, ca=cosf(a), sa=sinf(a);
    float hw=0.5f*BASE_W, hd=0.5f*BASE_D;
    float lx[4]={-hw,hw,hw,-hw}, ly[4]={-hd,-hd,hd,hd}, vx[4], vy[4];
    for(int k=0;k<4;k++){ vx[k]=sc->lz_x+lx[k]*ca-ly[k]*sa;
                          vy[k]=sc->lz_y+lx[k]*sa+ly[k]*ca; }
    float hp=getenv("VAT_HORDE_LZ_HP")?atof(getenv("VAT_HORDE_LZ_HP")):gBal.lz_hp;
    int sid=def_add_structure(g,hp,1);
    if(sid<0) return;
    int ccx=(int)(sc->lz_x/cell), ccy=(int)(sc->lz_y/cell);
    if(!simp_is_wall(s,ccx,ccy)) simp_set_goal(s,ccx,ccy,true);   // sorgente sigillata
    LzFootCtx fc={g,sid,ccx,ccy};
    scene_raster_cells(vx,vy,4,cell,gw,gh,lz_foot_cb,&fc);
    simp_terrain_commit(s);
    gLzCore=sid; gStructOn=1;
    gLzX=sc->lz_x; gLzY=sc->lz_y; gLzYaw=a;   // per il box container in build_struct_mesh
    gBaseOX=sc->lz_x; gBaseOY=sc->lz_y;       // il mortaio parte dal container
    gMortAz=a; gMortTilt=0.0f;                // tubo a riposo, lungo l'asse container
}

typedef struct { SimP *s; int n; } HoleCtx;
static void hole_wall_cb(void *u, int cx, int cy){
    HoleCtx *c = (HoleCtx*)u;
    simp_set_wall(c->s, cx, cy, true);
    simp_set_wall_cost(c->s, cx, cy, PALAZZO_WALL_MULT * simp_wall_base_cost());
    c->n++;
}

static int build_world(const Scene *sc, VatLayer *vl, int fillN, SpawnCtx *spctx,
                       SimP **ps, DefGame **pg, DefDirector **pdir){
    // tabella balance: default compilati + assets/balance.cfg, RILETTA a ogni
    // rebuild (cambio livello, EDIT->PLAY): si tara senza riavviare.
    balance_defaults(&gBal);
    { const char *bp=getenv("VAT_HORDE_BALANCE"); if(!bp) bp="assets/balance.cfg";
      int bbad=0, bn=balance_load(&gBal,bp,&bbad);
      if(bn>=0) printf("balance: %s, %d chiavi%s\n",bp,bn,
                       bbad?" (righe ignorate: vedi warning)":"");
      else printf("balance: %s assente, default compilati\n",bp); }
    host_apply_balance();
    SimP *s = scene_instantiate(sc, MAXA);
    if(!s){ fprintf(stderr,"scene_instantiate fail\n"); return -1; }
    spctx->s = s; spctx->vl = vl;
    if(vl) vat_layer_reset(vl);   // retry/rebuild: via cadaveri, decal, gib e
                                  // slot stantii (il SimP nuovo riusa gli handle)
    gScorchN = gScorchHead = 0;                     // crateri scorch: mondo pulito
#ifdef GAME_SHELL
    for(int i=0;i<STRIKE_MAX;i++) gStrikes[i].on = 0;                // reset mortaio
    gMortCd = 0.0f;                                                  // tubo freddo
    gHeli.active = 0; gLzHeld = 0;                                   // reset cinematica
    // serbatoio biomassa (BIOMASS_DESIGN v2 §3+§8): `biotank [start] [cap]`
    // della scena (omesso = vuoto, capienza di default). Verbi tutti spenti.
    bio_mode_set(0,0,0); gBioFlash = 0.0f;
    bio_init(&gBio, sc->bio_start, sc->bio_cap>0.0f?sc->bio_cap:gBal.bio.cap);
    // soldato giocabile: ricreato col mondo, def dalla tabella balance (il
    // vecchio è già morto in free_world — mai due corpi nello stesso SimP)
    { SoldierDef sd; soldier_def_defaults(&sd);
      sd.radius=gBal.soldier.radius;    sd.mass=gBal.soldier.mass;
      sd.speed=gBal.soldier.speed;      sd.accel=gBal.soldier.accel;
      sd.hp_max=gBal.soldier.hp;        sd.touch_dps=gBal.soldier.touch_dps;
      sd.touch_knock=gBal.soldier.touch_knock;
      sd.gun_range=gBal.soldier.gun_range;
      sd.gun_period=gBal.soldier.gun_period;
      sd.gun_damage=gBal.soldier.gun_damage;
      sd.lure_w=gBal.soldier.lure_w;    sd.lure_r=gBal.soldier.lure_r;
      sd.down_s=gBal.soldier.down_s;
      gSol = soldier_create(s, &sd);
      gSolMode=0; gSolFire=0; gSolWant=0; gGren.on=0; gSolBarOn=0; }
#endif
    gTurMagSeen = 0;                      // caricatori: ri-equipaggia le torrette

    // buchi del terreno (statici indistruttibili) PRIMA di prefill/base, così
    // gli agenti non nascono dentro un palazzo e il nav è già corretto.
    if(gTerOn){ HoleCtx hc={s,0};
        terrain_each_hole_cell(&gTer, sc->cell, simp_grid_w(s), simp_grid_h(s), hole_wall_cb, &hc);
        if(hc.n){ simp_terrain_commit(s);
            printf("statici terreno: %d celle-muro (tier palazzo) dai buchi ZHM2\n", hc.n); } }

    DefGame *g = def_create(s, MAXA);
    *def_tuning(g) = gBal.def;      // nemici/DoT/assedio/mix da balance.cfg
    def_set_event_cb(g, on_def_event, spctx);   // hit/death -> animazioni one-shot
    float bcx=sc->world_w*0.5f, bcy=sc->world_h*0.5f;
    if(sc->n_goal>0){ float sx=0,sy=0; for(int k=0;k<sc->n_goal;k++){
            sx+=sc->goal[k].x+sc->goal[k].w*0.5f; sy+=sc->goal[k].y+sc->goal[k].h*0.5f; }
        bcx=sx/sc->n_goal; bcy=sy/sc->n_goal; }
    gBaseOX=bcx; gBaseOY=bcy;                 // fallback origine mortaio (il container lo sovrascrive)
    float mn = sc->world_w<sc->world_h?sc->world_w:sc->world_h;
    float TR_R = 0.22f*mn;
    def_set_budget(g, getenv("VAT_HORDE_BUDGET")?atoi(getenv("VAT_HORDE_BUDGET")):gBal.budget);
    int placed=0;
    // a "designed" scene (walls, turrets, or a declared mission) owns its turrets: place
    // ONLY the scene's (possibly zero). A legacy scene gets the demo auto-ring.
    // a mission scene is authored: it owns its turrets (the `turret` lines, or
    // none) and must NOT get the legacy demo ring, which would carpet the LZ.
    int designed = (sc->n_wall>0 || sc->n_turret>0 || sc->mission.kind != SCENE_MISSION_NONE);
    // destructible turrets: a turret becomes a 1-cell solid the horde sieges to
    // reach the goal beyond -> exposed turrets in a breached ring get assaulted
    // and silenced (def_turret_make_destructible). HP from env, 0 = indestructible.
    float turret_hp_env = getenv("VAT_HORDE_TURRET_HP")?(float)atof(getenv("VAT_HORDE_TURRET_HP")):-1.0f;
    memset(gTurWasDead,0,sizeof gTurWasDead);         // mondo nuovo: nessun crollo visto
    int any_destr=0;                                  // almeno una torretta distruttibile
    // contact-siege tuning (def_set_turret_contact): 0 = keep default. Lets the
    // turrets be made tougher/weaker to the swarm at a glance, HP unchanged.
    // Reach di gioco 2.0 m (default defense 0.9): chi passa nel corridoio
    // adiacente morde la torretta — insieme al lure sotto, le torrette nel
    // flusso si pagano (2026-07-04).
    def_set_turret_contact(g,
        getenv("VAT_HORDE_TURRET_DPS")?atof(getenv("VAT_HORDE_TURRET_DPS")):gBal.contact.turret_dps,
        getenv("VAT_HORDE_TURRET_REACH")?atof(getenv("VAT_HORDE_TURRET_REACH")):gBal.contact.turret_reach);
    // richiamo da fuoco (def_set_fire_lure): le torrette che sparano ATTIRANO
    // l'orda (rumore) — cost_user negativo attorno finché sparano, rimozione
    // esatta al silenzio/crollo. VAT_HORDE_LURE="w,r,linger" (w>=0 = off).
    { float lw=gBal.lure.weight, lr=gBal.lure.radius, ll=gBal.lure.linger;
      if(getenv("VAT_HORDE_LURE")) sscanf(getenv("VAT_HORDE_LURE"),"%f,%f,%f",&lw,&lr,&ll);
      def_set_fire_lure(g,lw,lr,ll); }
    if(designed){
        for(int k=0;k<sc->n_turret;k++){ const SceneTurret *st=&sc->turret[k];
            DefTurret t={0};
            t.x=st->x; t.y=st->y;
            if(st->arc_deg>0.0f){                         // authored aim cone
                float fc=st->facing_deg*(3.14159265f/180.0f);
                float ha=st->arc_deg*(3.14159265f/360.0f);
                t.ang=fc; t.arc_min=fc-ha; t.arc_max=fc+ha;
                t.aim_tol=DEF_AIM_TOL_STD;                // turn-then-shoot
            } else {                                      // legacy: full sweep
                t.ang=0.0f; t.arc_min=-3.1416f; t.arc_max=3.1416f;
            }
            t.sweep_dir=1; t.sweep_speed=3.0f; t.range=st->range;
            // la colonna heavy della scena porta l'intero kind (0/1/2/3);
            // combat per-kind dalla tabella balance (stessa del piazzamento —
            // unificata 2026-07-15: la light di scena era 0.10s/55HP)
            t.kind=st->heavy; t.piercing=0;
            const BalTurret *bt=&gBal.tur[(t.kind>=0&&t.kind<4)?t.kind:0];
            t.fire_period=bt->fire_period; t.damage=bt->damage;
            if(t.range<=0.0f) t.range=bt->range;
            if(getenv("VAT_HORDE_TFIRE")) t.fire_period=atof(getenv("VAT_HORDE_TFIRE"));
            if(getenv("VAT_HORDE_TDMG"))  t.damage=atof(getenv("VAT_HORDE_TDMG"));
            int tid=def_add_turret(g,&t);
            // per-turret HP di scena, poi env (0 = indistruttibili), poi balance per kind
            float hp = st->hp>0.0f ? st->hp
                     : (turret_hp_env>=0.0f ? turret_hp_env
                                            : gBal.tur[(t.kind>=0&&t.kind<4)?t.kind:0].hp);
            if(hp>0.0f){ def_turret_make_destructible(g,tid,hp); any_destr=1; }
            placed++; }
        if(any_destr) simp_terrain_commit(s);             // commit turret walls
        printf("torrette (scena): %d%s\n", placed,
               any_destr?" (distruttibili)":"");
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

    // prop solidi/assediabili dal catalogo (ENTITY_DESIGN §6+§8.5,
    // prop_world_apply): footprint W×D → celle nav + opacità + strutture per
    // gli hp finiti. PRIMA del prefill/director: niente spawn dentro un bus.
    prop_world_apply(sc, &gCatalog, s, g, &gPropW);
    if(gPropW.n_solid>0)
        printf("prop solidi (§6): %d footprint nav, %d assediabili\n",
               gPropW.n_solid, gPropW.n_siege);

    if(fillN){ int got=prefill_lattice(s,g,vl,sc,fillN);
        printf("prefill: target %d -> %d agenti piazzati\n", fillN, got); }

    // missione dal .scn (fase A): PRIMA del director legacy — se la scena la
    // dichiara, i director (uno per exit) li possiede la missione e il legacy
    // non parte. LZ -> core assediabile + goal. Il vecchio gMission (rebuild
    // EDIT->PLAY / cambio livello) si smonta qui: tocca solo i suoi malloc.
    if(gMissionOn){ mission_destroy(&gMission); gMissionOn=0; }
    gMissionOn = (mission_create(&gMission, sc, s, g, on_director_spawn, spctx)==0);
    if(gMissionOn){
        gMission.wave_pause = gBal.wave.pause;   // LOOP A: annuncio da balance
        gWaveMsgT = 0.0f;
        if(sc->has_lz) build_lz_core(g,s,sc);
        char pool[24]="inf";
        if(gMission.pool_total>0) snprintf(pool,sizeof pool,"%d",gMission.pool_total);
        printf("missione: %s %.0fs prep %s, %d exit (pool %s), budget %d%s",
               gMission.kind==SCENE_MISSION_SURVIVE?"SURVIVE":"CLEAR",
               (double)gMission.survive_s,
               gMission.prep_s>0.0f?"a tempo":"illimitata (INVIO=via)",
               sc->n_exit, pool, def_budget(g), sc->has_lz?" + LZ":"");
        if(mission_wave_total(&gMission)>0)
            printf(", %d ondate (pausa %.0fs, INVIO=chiama)",
                   mission_wave_total(&gMission),(double)gMission.wave_pause);
        printf("\n");
    }

    DefRect drects[16]; int ndr=sc->n_spawn<16?sc->n_spawn:16;
    for(int k=0;k<ndr;k++){ drects[k].x=sc->spawn[k].x; drects[k].y=sc->spawn[k].y;
        drects[k].w=sc->spawn[k].w; drects[k].h=sc->spawn[k].h; }
    DefDirector *dir=NULL;
    if(!gMissionOn && !fillN && ndr>0){ DefDirectorCfg dc={0};
        dc.rects=drects; dc.nrects=ndr; dc.spawn_radius=0.34f;
        dc.base_rate=getenv("VAT_HORDE_RATE")?atof(getenv("VAT_HORDE_RATE")):gBal.director.base_rate;
        dc.rate_ramp=gBal.director.rate_ramp; dc.wave_period=gBal.director.wave_period;
        dc.seed=0x5EED1234u;
        dc.on_spawn=on_director_spawn; dc.user=spctx;
        dir=def_director_create(g,&dc);
        printf("director: %d rect, base %.0f/s +%.0f/ondata (%.0fs)\n",ndr,
               (double)dc.base_rate,(double)dc.rate_ramp,(double)dc.wave_period); }

    *ps=s; *pg=g; *pdir=dir; return 0;
}

static void free_world(SimP *s, DefGame *g, DefDirector *dir){
#ifdef GAME_SHELL
    if(gSol){ soldier_destroy(gSol); gSol=NULL; }   // prima del SimP: il recall
                                                    // rimuove corpo e lure
#endif
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

#ifdef GAME_SHELL
// ============================ GAME SHELL =================================
// (GAME_APP_DESIGN.md §4) Il flusso vive in app.c (logica pura, testata da
// test_app); qui c'è solo l'ESECUZIONE delle azioni (caricare il livello,
// avviare le fasi, salvare) e il disegno degli overlay con font8.
// (gApp è dichiarata sopra, accanto al modulo heli che la legge)
static float gSurviveT=0.0f;            // orologio dell'assalto
static int   gShellCore=-1;             // id struttura core LZ del livello

// Core della LZ (placeholder dell'entità `lz` di GAME_PLAN fase A): anello di
// celle-muro attorno al centroide dei goal, is_core -> def_lost = sconfitta.
// I goal dentro l'anello restano drenanti: l'orda che sfonda arriva a spegnerli
// solo passando dal core.
static void shell_build_core(DefGame *g, SimP *s, const Scene *sc, float hp){
    gShellCore=-1;
    if(hp<=0.0f || sc->n_goal<=0) return;
    float sx=0,sy=0;
    for(int k=0;k<sc->n_goal;k++){ sx+=sc->goal[k].x+sc->goal[k].w*0.5f;
                                   sy+=sc->goal[k].y+sc->goal[k].h*0.5f; }
    int cx0=(int)(sx/(float)sc->n_goal/sc->cell), cy0=(int)(sy/(float)sc->n_goal/sc->cell);
    int id=def_add_structure(g,hp,1), h=2;                 // anello 5x5
    if(id<0) return;
    for(int cy=cy0-h;cy<=cy0+h;cy++)for(int cx=cx0-h;cx<=cx0+h;cx++)
        if(cx==cx0-h||cx==cx0+h||cy==cy0-h||cy==cy0+h) def_struct_cell(g,id,cx,cy);
    simp_terrain_commit(s);
    gShellCore=id; gStructOn=1;
}

// Puntatori allo stato di main() di cui il cambio-livello ha bisogno: la shell
// è un layer, non una ristrutturazione del monolite (si separa in fase G).
typedef struct {
    Scene *sc; SimP **s; DefGame **g; DefDirector **dir;
    VatLayer *vl; SpawnCtx *spctx; Destruct *dz;
    GLuint obVbo, prVbo; int *obNV, *prNV; int ground_on;
    float *cam_x, *cam_z, *cam_hh; int *running;
    SDL_Window *win; int *fullscreen;   // per APP_SET_FULLSCREEN (apply live)
    Placement *plc; Traps *traps;
} ShellHost;
static ShellHost gHost;

// APP_ACT_LOAD_LEVEL: caricamento SINCRONO (v1) nello stato BRIEFING —
// il punto d'aggancio del preload asincrono vero (GAME_APP_DESIGN §6).
static void shell_load_level(void){
    const AppLevel *L=app_cur_level(&gApp);
    if(!L){ *gHost.running=0; return; }
    scene_free(gHost.sc);
    if(scene_load(L->scene,gHost.sc)!=0){
        fprintf(stderr,"livello %d: scene load fail: %s\n",gApp.cur,L->scene);
        *gHost.running=0; return; }
    free_world(*gHost.s,*gHost.g,*gHost.dir);
    if(build_world(gHost.sc,gHost.vl,0,gHost.spctx,gHost.s,gHost.g,gHost.dir)!=0){
        *gHost.running=0; return; }
    // fase A: se la scena dichiara missione + lz, il core l'ha già alzato
    // build_lz_core dentro build_world — niente secondo anello dai goal.
    if(!(gMissionOn && gHost.sc->has_lz))
        shell_build_core(*gHost.g,*gHost.s,gHost.sc,L->core_hp);
    destruct_init(gHost.dz,gHost.sc,&gCatalog);
    *gHost.obNV=upload_obstacle_mesh(gHost.obVbo,gHost.sc,!gHost.ground_on);
    *gHost.prNV=upload_prop_mesh(gHost.prVbo,gHost.sc,&gCatalog,gHost.dz,*gHost.g);
    *gHost.cam_x=gHost.sc->world_w*0.5f; *gHost.cam_z=gHost.sc->world_h*0.5f;
    anim_init(&gAnim); gSurviveT=0.0f;
    if(gHost.plc){ pl_undo_clear(gHost.plc);   // mondo nuovo: record stantii via
                   gHost.plc->active=0; }
    if(gHost.traps) traps_init(gHost.traps);   // mondo nuovo: via le mine del livello prima
    plmod_clear();                             // registro moduli render idem
    printf("livello %d (%s): %s pronto\n",gApp.cur+1,L->name,L->scene);
    app_level_ready(&gApp);
}

// musica di battaglia: cerca il primo formato presente in assets/music/
// (l'utente esporta il tema da Strudel). FLAC/MP3/WAV = decoder built-in di
// miniaudio; OGG solo se scaricato stb_vorbis (vedi audio.c / Makefile). File
// mancante o formato non gestito = si prova il prossimo; nessuno = silenzio,
// nessun crash (au_music_play torna -1). base = path SENZA estensione.
static int shell_music_start(const char *base, int loop){
    static const char *ext[]={".ogg",".flac",".mp3",".wav"};
    char path[192];
    for(int i=0;i<4;i++){
        snprintf(path,sizeof path,"%s%s",base,ext[i]);
        if(au_music_play(path,loop)==0){ printf("musica: %s\n",path); return 0; }
    }
    fprintf(stderr,"musica: nessun file %s.{ogg,flac,mp3,wav} (silenzio)\n",base);
    return -1;
}

static void shell_do_act(AppAction act){
    switch(act){
        case APP_ACT_QUIT: *gHost.running=0; break;
        case APP_ACT_SAVE:
            if(app_save_progress(&gApp,"progress.txt")!=0)
                fprintf(stderr,"progress.txt: save fail\n");
            /* fallthrough: il salvataggio arriva dai settings o da un esito,
               in entrambi i casi i volumi vanno riapplicati */
            /* FALLTHROUGH */
        case APP_ACT_APPLY_SETTINGS:
            au_set_volume((float)gApp.vol_sfx/10.0f,(float)gApp.vol_music/10.0f);
            // schermo intero: applicato SUBITO (anche dalla schermata opzioni)
            if(gHost.win && gHost.fullscreen && *gHost.fullscreen!=gApp.fullscreen){
                *gHost.fullscreen=gApp.fullscreen;
                SDL_SetWindowFullscreen(gHost.win,gApp.fullscreen); }
            break;
        case APP_ACT_LOAD_LEVEL:    shell_load_level(); break;
        case APP_ACT_START_ASSAULT: gSurviveT=0.0f;
            if(gMissionOn) mission_go(&gMission);            // fase A: via ai director
            if(gHost.plc){ pl_undo_clear(gHost.plc);         // via = impegno (§6)
                           gHost.plc->active=0; }
            au_play(SND_ASSAULT);
            shell_music_start("assets/music/marcia_dell_orda",1);   // tema in loop
            break;
        case APP_ACT_START_PREP:    /* gating della sim legge lo stato */ break;
        case APP_ACT_START_DEPLOY:  // cinematica di consegna (BASE_DESIGN §4.1):
            // parte solo con lz + modello chinook; altrimenti si salta subito
            // in PREP (stesso percorso dello skip con INVIO).
            if(gHost.sc->has_lz && gHeliM.ok && gLzCore>=0){
                heli_begin(1,gHost.sc);
                // niente snap: la camera converge sull'heli col follow (vedi
                // blocco cinematica prima dei vincoli GAME_CAM_*)
            } else shell_do_act(app_input(&gApp,APP_IN_CONFIRM));
            break;
        default: break;
    }
}

// ---- overlay 2D: quad + testo nel flat.vs + ui.fs (alpha in aNormal.x,
// UV dell'atlas font in aNormal.yz — un solo shader e un solo draw call).
// Font UI VETTORIALE (2026-07-12): assets/fonts/ui.zfnt (atlas A8 + metriche,
// bakato da un TTF con gfx/font_bake.py — formato in testa allo script).
// ui_text scala i glifi così che la CAP-HEIGHT combaci con la vecchia cella
// font8 (FONT8_H·s): le taglie e i layout esistenti restano validi. Se il
// .zfnt manca si torna al font8 bitmap; i quad pieni puntano al blocco
// bianco riservato dell'atlas (fallback: texture 1×1 bianca).
typedef struct { unsigned cp; float adv;
                 unsigned short x,y,w,h; short ox,oy; } UiGlyph;
#define UIF_BOLD 0x10000u          // bit di faccia nel codepoint (vedi font_bake.py)
static struct {
    int ok; unsigned aw,ah,ng;
    float px,cap_top,cap_h,ascent,descent;
    float wu,wv;                    // UV del blocco bianco (quad pieni)
    UiGlyph *g;
    int idx[128],idx_b[128];        // ASCII diretto (regular/bold); accenti in scansione
    GLuint tex;
} gUiFont;
static int gUiBold=0;               // faccia corrente di ui_text (0 reg, 1 bold)
static const UiGlyph* ui_font_glyph(unsigned cp){
    if(cp<(128u|UIF_BOLD)){
        if(cp<128){ int k=gUiFont.idx[cp]; return k>=0?&gUiFont.g[k]:NULL; }
        if((cp&UIF_BOLD) && (cp&~UIF_BOLD)<128){
            int k=gUiFont.idx_b[cp&~UIF_BOLD]; return k>=0?&gUiFont.g[k]:NULL; }
    }
    for(unsigned i=0;i<gUiFont.ng;i++) if(gUiFont.g[i].cp==cp) return &gUiFont.g[i];
    return NULL;
}
static const UiGlyph* ui_font_pick(unsigned cp){   // faccia corrente + fallback regular
    const UiGlyph *gl=ui_font_glyph(cp|(gUiBold?UIF_BOLD:0u));
    return (gl||!gUiBold)?gl:ui_font_glyph(cp);
}
static void ui_font_load(const char *path){
    memset(&gUiFont,0,sizeof gUiFont);
    for(int i=0;i<128;i++){ gUiFont.idx[i]=-1; gUiFont.idx_b[i]=-1; }
    gUiFont.wu=gUiFont.wv=0.5f;
    unsigned char *buf=NULL; long len=0;
    FILE *f=fopen(path,"rb");
    if(f){ fseek(f,0,SEEK_END); len=ftell(f); fseek(f,0,SEEK_SET);
        buf=malloc((size_t)len);
        if(!buf || fread(buf,1,(size_t)len,f)!=(size_t)len){ free(buf); buf=NULL; }
        fclose(f); }
    glGenTextures(1,&gUiFont.tex); glBindTexture(GL_TEXTURE_2D,gUiFont.tex);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    if(buf && len>=44 && !memcmp(buf,"ZFN1",4)){   /* header ZFN1 = 44 byte */
        const unsigned char *p=buf+4;
        memcpy(&gUiFont.aw,p,4); memcpy(&gUiFont.ah,p+4,4); p+=8;
        memcpy(&gUiFont.px,p,4); memcpy(&gUiFont.cap_top,p+4,4);
        memcpy(&gUiFont.cap_h,p+8,4); memcpy(&gUiFont.ascent,p+12,4);
        memcpy(&gUiFont.descent,p+16,4); p+=20;
        unsigned wx,wy; memcpy(&wx,p,4); memcpy(&wy,p+4,4); p+=8;
        memcpy(&gUiFont.ng,p,4); p+=4;
        size_t need=44u+(size_t)gUiFont.ng*20u+(size_t)gUiFont.aw*gUiFont.ah;
        if(gUiFont.ng>0 && gUiFont.cap_h>0.0f && (size_t)len>=need){
            gUiFont.g=malloc(gUiFont.ng*sizeof(UiGlyph));
            for(unsigned i=0;i<gUiFont.ng;i++){ UiGlyph *gl=&gUiFont.g[i];
                memcpy(&gl->cp,p,4); memcpy(&gl->adv,p+4,4);
                memcpy(&gl->x,p+8,2); memcpy(&gl->y,p+10,2);
                memcpy(&gl->w,p+12,2); memcpy(&gl->h,p+14,2);
                memcpy(&gl->ox,p+16,2); memcpy(&gl->oy,p+18,2); p+=20;
                if(gl->cp<128) gUiFont.idx[gl->cp]=(int)i;
                else if((gl->cp&UIF_BOLD) && (gl->cp&~UIF_BOLD)<128)
                    gUiFont.idx_b[gl->cp&~UIF_BOLD]=(int)i; }
            glPixelStorei(GL_UNPACK_ALIGNMENT,1);
            glTexImage2D(GL_TEXTURE_2D,0,GL_R8,(GLsizei)gUiFont.aw,(GLsizei)gUiFont.ah,
                         0,GL_RED,GL_UNSIGNED_BYTE,p);
            glGenerateMipmap(GL_TEXTURE_2D);
            glPixelStorei(GL_UNPACK_ALIGNMENT,4);
            gUiFont.wu=((float)wx+0.5f)/(float)gUiFont.aw;
            gUiFont.wv=((float)wy+0.5f)/(float)gUiFont.ah;
            gUiFont.ok=1;
            printf("font UI: %s (atlas %ux%u, %u glifi, cap %.1f px)\n",
                   path,gUiFont.aw,gUiFont.ah,gUiFont.ng,(double)gUiFont.cap_h);
        }
    }
    if(!gUiFont.ok){                       // fallback: 1×1 bianca -> font8 bitmap
        unsigned char w8=255;
        glPixelStorei(GL_UNPACK_ALIGNMENT,1);
        glTexImage2D(GL_TEXTURE_2D,0,GL_R8,1,1,0,GL_RED,GL_UNSIGNED_BYTE,&w8);
        glGenerateMipmap(GL_TEXTURE_2D);
        glPixelStorei(GL_UNPACK_ALIGNMENT,4);
        fprintf(stderr,"font UI: %s assente/invalido, fallback font8 bitmap\n",path);
    }
    free(buf);
}
#define UI_MAX_V 120000
static float *gUiBuf=NULL; static int gUiV=0;
static void ui_quad_uv(float x,float y,float w,float h,
                       float u0,float v0,float u1,float v1,
                       float r,float g,float b,float a){
    if(gUiV+6>UI_MAX_V) return;
    float v[6][4]={{x,y,u0,v0},{x+w,y,u1,v0},{x+w,y+h,u1,v1},
                   {x,y,u0,v0},{x+w,y+h,u1,v1},{x,y+h,u0,v1}};
    for(int i=0;i<6;i++){ float *o=gUiBuf+(size_t)(gUiV+i)*9;
        o[0]=v[i][0];o[1]=v[i][1];o[2]=0.0f;
        o[3]=a;o[4]=v[i][2];o[5]=v[i][3]; o[6]=r;o[7]=g;o[8]=b; }
    gUiV+=6;
}
static void ui_quad(float x,float y,float w,float h,float r,float g,float b,float a){
    ui_quad_uv(x,y,w,h,gUiFont.wu,gUiFont.wv,gUiFont.wu,gUiFont.wv,r,g,b,a);
}
// decodifica UTF-8 minima (2 byte: bastano per gli accenti del charset bakato)
static unsigned ui_utf8_next(const unsigned char **pp){
    unsigned c=*(*pp)++;
    if(c>=0xC0 && c<0xE0 && **pp){ c=((c&0x1Fu)<<6)|(**pp&0x3Fu); (*pp)++; }
    return c;
}
static void ui_text(float x,float y,float s,const char*str,float r,float g,float b,float a){
    if(gUiFont.ok){
        float sc=s*FONT8_H/gUiFont.cap_h;      // cap-height = vecchia cella font8
        float iw=1.0f/(float)gUiFont.aw, ih=1.0f/(float)gUiFont.ah;
        float cx=x;
        for(const unsigned char*p=(const unsigned char*)str;*p;){
            unsigned cp=ui_utf8_next(&p);
            if(cp=='\n'){ y+=s*(FONT8_H+3); cx=x; continue; }
            const UiGlyph *gl=ui_font_pick(cp);
            if(!gl){ cx+=s*FONT8_ADV; continue; }
            if(gl->w)
                ui_quad_uv(cx+(float)gl->ox*sc, y+((float)gl->oy-gUiFont.cap_top)*sc,
                           (float)gl->w*sc,(float)gl->h*sc,
                           (float)gl->x*iw,(float)gl->y*ih,
                           (float)(gl->x+gl->w)*iw,(float)(gl->y+gl->h)*ih,
                           r,g,b,a);
            cx+=gl->adv*sc;
        }
        return;
    }
    float cx=x;
    for(const char*p=str;*p;p++){
        if(*p=='\n'){ y+=s*(FONT8_H+3); cx=x; continue; }
        const unsigned char *gl=font8_glyph((unsigned char)*p);
        for(int ry=0;ry<FONT8_H;ry++)for(int rx=0;rx<FONT8_W;rx++)
            if((gl[ry]>>(4-rx))&1) ui_quad(cx+rx*s,y+ry*s,s,s,r,g,b,a);
        cx+=s*FONT8_ADV;
    }
}
static float ui_text_w(float s,const char*str){        // larghezza riga più lunga
    if(gUiFont.ok){
        float sc=s*FONT8_H/gUiFont.cap_h, wl=0.0f, best=0.0f;
        for(const unsigned char*p=(const unsigned char*)str;*p;){
            unsigned cp=ui_utf8_next(&p);
            if(cp=='\n'){ if(wl>best)best=wl; wl=0.0f; continue; }
            const UiGlyph *gl=ui_font_pick(cp);
            wl += gl ? gl->adv*sc : s*FONT8_ADV;
        }
        if(wl>best)best=wl;
        return best;
    }
    int n=0,best=0;
    for(const char*p=str;*p;p++){ if(*p=='\n'){ if(n>best)best=n; n=0; } else n++; }
    if(n>best)best=n;
    return (float)best*s*FONT8_ADV;
}
static void ui_text_c(float cxpx,float y,float s,const char*str,float r,float g,float b,float a){
    ui_text(cxpx-ui_text_w(s,str)*0.5f,y,s,str,r,g,b,a);
}
// variante BOLD centrata (seconda faccia dell'atlas; senza bold nel .zfnt
// cade sul regular). Per bold non centrato: gUiBold=1 attorno a ui_text.
static void ui_text_cb(float cxpx,float y,float s,const char*str,float r,float g,float b,float a){
    gUiBold=1; ui_text_c(cxpx,y,s,str,r,g,b,a); gUiBold=0;
}

// ---- immagini UI (title screen, pulsanti menu): PNG RGBA via stb_image,
// disegnate con flat.vs + uiimg.fs PRIMA del buffer quad/testo (che quindi
// ci scrive sopra). Coda per-frame, pochi elementi: un draw call ciascuna.
static struct { GLuint title,btn,btn_hov; int tw,th,bw,bh; } gUiTex;
#define UI_IMG_MAX 16
static struct { GLuint tex; float x,y,w,h,r,g,b,a; } gUiImg[UI_IMG_MAX];
static int gUiImgN=0;
static GLuint ui_tex_load(const char *path,int *ow,int *oh,int required){
    int w,h,n; unsigned char *d=stbi_load(path,&w,&h,&n,4);
    if(!d){ if(required) fprintf(stderr,"ui: %s assente/illeggibile\n",path); return 0; }
    GLuint t; glGenTextures(1,&t); glBindTexture(GL_TEXTURE_2D,t);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,d);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    stbi_image_free(d);
    if(ow)*ow=w; if(oh)*oh=h;
    printf("ui: %s %dx%d\n",path,w,h);
    return t;
}
static void ui_img(GLuint tex,float x,float y,float w,float h,
                   float r,float g,float b,float a){
    if(!tex || gUiImgN>=UI_IMG_MAX) return;
    gUiImg[gUiImgN].tex=tex;
    gUiImg[gUiImgN].x=x; gUiImg[gUiImgN].y=y; gUiImg[gUiImgN].w=w; gUiImg[gUiImgN].h=h;
    gUiImg[gUiImgN].r=r; gUiImg[gUiImgN].g=g; gUiImg[gUiImgN].b=b; gUiImg[gUiImgN].a=a;
    gUiImgN++;
}

// ---- barra PREP (PREP_UI_DESIGN §3-§4): 3 tab filtro su PlKind, card voce,
// pannello info, undo, VIA ALL'ORDA. Layout ricomputato on demand (render E
// hit test chiamano prep_ui_layout: mai rect stantii, mai commit sotto la UI).
typedef struct { float x,y,w,h; } UiRect;
static int ui_hit(const UiRect *r,float mx,float my){
    return mx>=r->x && mx<r->x+r->w && my>=r->y && my<r->y+r->h; }

enum { PREP_TABS=3, PREP_TAB_ITEMS=8 };
typedef struct { const char *label; PlKind kind; int items[PREP_TAB_ITEMS]; int n; } PrepTab;
static PrepTab gPrepTabs[PREP_TABS]={ {"1 TORRETTE",PL_TURRET,{0},0},
                                      {"2 BARRIERE",PL_BARRICADE,{0},0},
                                      {"3 TRAPPOLE",PL_TRAP,{0},0} };
static int   gPrepTab=0;
static float gPrepBarY=1e9f;                       // sopra = mondo, sotto = UI
static UiRect gPrepTabR[PREP_TABS], gPrepCardR[PREP_TAB_ITEMS], gPrepUndoR, gPrepGoR;
static UiRect gPrepAdjR;                           // REGOLA (V) anche in PREP
// ghost di linea (drag in corso): il blocco ghost del frame scrive qui, la
// barra (disegnata dopo) legge per il costo live accanto al cursore (§5).
static int gLineOn=0, gLineValid=0, gLineCost=0; static float gLineLen=0.0f;

static void prep_tabs_build(const Placement *p){   // indice tab->voci, una volta
    for(int t=0;t<PREP_TABS;t++) gPrepTabs[t].n=0;
    for(int i=0;i<p->ncat;i++)
        for(int t=0;t<PREP_TABS;t++)
            if(p->cat[i].kind==gPrepTabs[t].kind && gPrepTabs[t].n<PREP_TAB_ITEMS)
                gPrepTabs[t].items[gPrepTabs[t].n++]=i;
    gPrepTab=0;
}
// cambio tab: selezione sulla prima voce affordabile (o la prima) — §4.
static void prep_tab_set(int t,DefGame *g){
    if(t<0||t>=PREP_TABS||gPrepTabs[t].n<=0) return;   // TRAPPOLE vuota: resta
    gPrepTab=t;
    Placement *p=gHost.plc;
    int pick=gPrepTabs[t].items[0];
    for(int i=0;i<gPrepTabs[t].n;i++){
        int idx=gPrepTabs[t].items[i];
        if(def_budget(g)>=p->cat[idx].cost){ pick=idx; break; }
    }
    pl_select(p,pick);
}
static void prep_cycle(int dir){                   // [ ] = voce prec/succ NELLA tab
    PrepTab *tb=&gPrepTabs[gPrepTab];
    if(tb->n<=0) return;
    Placement *p=gHost.plc;
    int pos=0;
    for(int i=0;i<tb->n;i++) if(tb->items[i]==p->sel){ pos=i; break; }
    pos=((pos+dir)%tb->n+tb->n)%tb->n;
    pl_select(p,tb->items[pos]);
}
#define PREP_BAR_H   110.0f
#define PREP_CARD_W  104.0f
#define PREP_CARD_H   62.0f
static void prep_ui_layout(int SW,int SH){
    float y0=(float)SH-PREP_BAR_H; gPrepBarY=y0;
    float x=10.0f;
    for(int t=0;t<PREP_TABS;t++){                   // riga tab
        float w=ui_text_w(2,gPrepTabs[t].label)+20.0f;
        gPrepTabR[t]=(UiRect){x,y0+6,w,22}; x+=w+8.0f;
    }
    gPrepUndoR=(UiRect){x+12,y0+6,ui_text_w(2,"ANNULLA")+20.0f,22};
    // REGOLA anche in PREP (BIOMASS §12.Q3): sbagliare il facing al piazzamento
    // non deve costare un undo. Gratis, stesso gesto dell'assalto.
    gPrepAdjR=(UiRect){gPrepUndoR.x+gPrepUndoR.w+8,y0+6,ui_text_w(2,"REGOLA (V)")+20.0f,22};
    x=10.0f;
    for(int c=0;c<PREP_TAB_ITEMS;c++){              // riga card (tab attiva)
        gPrepCardR[c]=(UiRect){x,y0+36,PREP_CARD_W,PREP_CARD_H}; x+=PREP_CARD_W+8.0f; }
    float gw=ui_text_w(3,"VIA ALL'ORDA")+28.0f;
    gPrepGoR=(UiRect){(float)SW-gw-12,y0+62,gw,34};
}
// icona placeholder della card (§2: quadratino evocativo, niente pipeline icone)
static void prep_icon(const PlItem *it,float cx,float cy,float dim){
    float a=dim;
    if(it->kind==PL_TURRET){
        // tinta per tipo: leggera azzurra, pesante arancio, fiamme rosso
        // fuoco, acido verde (stessa mappa cromatica delle canne 3D)
        float r=0.55f,g=0.75f,b=0.90f;
        if(it->heavy==1){ r=0.85f;g=0.45f;b=0.20f; }
        else if(it->heavy==2){ r=0.95f;g=0.35f;b=0.08f; }
        else if(it->heavy==3){ r=0.35f;g=0.85f;b=0.25f; }
        ui_quad(cx-9,cy-7,18,14,r*0.5f,g*0.5f,b*0.5f,a);          // base
        ui_quad(cx-2,cy-15,4,10,r,g,b,a);                          // canna
    } else if(it->kind==PL_TRAP){                                  // mina
        ui_quad(cx-11,cy-3,22,10,0.28f,0.30f,0.28f,a);            // corpo basso
        ui_quad(cx-3,cy-9,6,6,0.90f,0.72f,0.15f,a);              // pomello a pressione
    } else if(it->opacity>0.0f && it->opacity<1.0f){               // cancellata
        for(int k=0;k<5;k++) ui_quad(cx-13+k*6,cy-11,3,22,0.72f,0.74f,0.80f,a);
        ui_quad(cx-14,cy-12,29,3,0.72f,0.74f,0.80f,a);
    } else {                                                       // barricata
        ui_quad(cx-14,cy-6,28,12,0.62f,0.44f,0.20f,a);
        ui_quad(cx-14,cy-10,28,3,0.72f,0.54f,0.28f,a);
    }
}
// pannello info: 1-2 stat leggibili per la voce selezionata (§3)
static void prep_info_lines(const PlItem *it,char *l1,int n1,char *l2,int n2){
    if(it->kind==PL_TURRET){
        // default per tipo allineati a place.c commit_turret
        static const char *tn[4]={"TORRETTA LEGGERA","TORRETTA PESANTE",
                                  "LANCIAFIAMME","LANCIA ACIDO"};
        static const float td[4][2]={{40.0f,0.12f},{40.0f,0.5f},
                                     {12.0f,0.15f},{18.0f,0.25f}};
        int k=(it->heavy>=0&&it->heavy<4)?it->heavy:0;
        float rng=it->range>0?it->range:td[k][0];
        float per=it->fire_period>0?it->fire_period:td[k][1];
        snprintf(l1,n1,"%s - %d$",tn[k],it->cost);
        const char *fx = (k==1)?" - SMEMBRA":(k==2)?" - INCENDIA (AREA)"
                         :(k==3)?" - SCIOGLIE (AREA)":"";
        snprintf(l2,n2,"GITTATA %.0f M - %.1f COLPI/S%s",(double)rng,1.0/per,fx);
    } else if(it->kind==PL_TRAP){
        snprintf(l1,n1,"MINA - %d$",it->cost);
        snprintf(l2,n2,"ESPLODE AL CONTATTO - RAGGIO %.0f M",
                 (double)(it->blast_r>0?it->blast_r:6.0f));
    } else {
        int semi=(it->opacity>0.0f && it->opacity<1.0f);
        snprintf(l1,n1,"%s - %d$/M - %.0f HP PER MODULO",it->name,it->cost,(double)it->hp);
        snprintf(l2,n2,semi?"TRASCINA UNA LINEA - LE TORRETTE SPARANO ATTRAVERSO"
                           :"TRASCINA UNA LINEA COL MOUSE");
    }
}
// click LMB dentro la barra: tab/card/undo/via. Torna 1 = consumato (mai mondo).
static int prep_ui_click(float mx,float my,int SW,int SH,DefGame *g,SimP *s){
    prep_ui_layout(SW,SH);
    if(my<gPrepBarY) return 0;
    Placement *p=gHost.plc;
    for(int t=0;t<PREP_TABS;t++)
        if(ui_hit(&gPrepTabR[t],mx,my)){ prep_tab_set(t,g); return 1; }
    if(ui_hit(&gPrepUndoR,mx,my)){
        if(pl_undo_pop(p,g,s)) gStructOn=1;
        plmod_trim(g); return 1; }
    if(ui_hit(&gPrepAdjR,mx,my)){                   // REGOLA (V): esclusiva col ghost
        bio_mode_set(0,0,!gAdjOn);
        if(gAdjOn) p->active=0;
        au_play(SND_MENU_SELECT); return 1; }
    if(ui_hit(&gPrepGoR,mx,my)){                    // = INVIO (APP_IN_CONFIRM)
        au_play(SND_MENU_SELECT);
        shell_do_act(app_input(&gApp,APP_IN_CONFIRM)); return 1; }
    PrepTab *tb=&gPrepTabs[gPrepTab];
    for(int c=0;c<tb->n;c++)
        if(ui_hit(&gPrepCardR[c],mx,my)){           // card = seleziona E attiva
            pl_select(p,tb->items[c]); p->active=1; return 1; }
    return 1;                                       // sfondo barra: consumato
}
static void prep_bar_draw(int SW,int SH,DefGame *g,float mpx,float mpy){
    prep_ui_layout(SW,SH);
    Placement *p=gHost.plc;
    const PlItem *sel=pl_selected(p);
    float y0=gPrepBarY;
    ui_quad(0,y0,(float)SW,PREP_BAR_H,0.05f,0.05f,0.09f,0.88f);
    ui_quad(0,y0,(float)SW,2,0.35f,0.10f,0.08f,1);
    for(int t=0;t<PREP_TABS;t++){                   // riga tab
        const UiRect *r=&gPrepTabR[t];
        int on=(t==gPrepTab), dead=(gPrepTabs[t].n==0);
        ui_quad(r->x,r->y,r->w,r->h,on?0.45f:0.14f,on?0.10f:0.14f,on?0.08f:0.20f,0.9f);
        float c=dead?0.35f:1.0f;
        ui_text(r->x+10,r->y+4,2,gPrepTabs[t].label,c,c,c,1);
        if(dead) ui_text(r->x+10,r->y+r->h+2,1,"PRESTO",0.5f,0.5f,0.4f,1);
    }
    { const UiRect *r=&gPrepUndoR;                  // undo (§6)
      int can=(p->undo && p->undo->n>0); float c=can?1.0f:0.4f;
      ui_quad(r->x,r->y,r->w,r->h,0.14f,0.14f,0.20f,0.9f);
      ui_text(r->x+10,r->y+4,2,"ANNULLA",c,c,c,1); }
    { const UiRect *r=&gPrepAdjR;                   // REGOLA (BIOMASS §4)
      ui_quad(r->x,r->y,r->w,r->h,gAdjOn?0.45f:0.14f,gAdjOn?0.28f:0.14f,
              gAdjOn?0.10f:0.20f,0.9f);
      ui_text(r->x+10,r->y+4,2,"REGOLA (V)",1,1,1,1); }
    { char bt[48]; snprintf(bt,sizeof bt,"BUDGET %d $",def_budget(g));  // §3
      int poor=(sel && def_budget(g)<sel->cost);
      ui_text((float)SW-ui_text_w(3,bt)-12,y0+9,3,bt,
              poor?0.95f:0.55f,poor?0.25f:0.95f,poor?0.20f:0.45f,1); }
    PrepTab *tb=&gPrepTabs[gPrepTab];
    for(int c=0;c<tb->n;c++){                       // card della tab attiva
        const UiRect *r=&gPrepCardR[c];
        const PlItem *it=&p->cat[tb->items[c]];
        int on=(p->active && tb->items[c]==p->sel);
        int aff=(def_budget(g)>=it->cost);
        ui_quad(r->x,r->y,r->w,r->h,0.12f,0.12f,0.17f,0.92f);
        if(on){ ui_quad(r->x,r->y,r->w,2,0.30f,0.95f,0.35f,1);
                ui_quad(r->x,r->y+r->h-2,r->w,2,0.30f,0.95f,0.35f,1);
                ui_quad(r->x,r->y,2,r->h,0.30f,0.95f,0.35f,1);
                ui_quad(r->x+r->w-2,r->y,2,r->h,0.30f,0.95f,0.35f,1); }
        prep_icon(it,r->x+r->w*0.5f,r->y+22,aff?1.0f:0.35f);
        float tc=aff?0.95f:0.45f;
        ui_text_c(r->x+r->w*0.5f,r->y+38,1.5f,it->name,tc,tc,tc,1);
        char cs[24]; snprintf(cs,sizeof cs,it->kind==PL_BARRICADE?"%d$/M":"%d$",it->cost);
        ui_text_c(r->x+r->w*0.5f,r->y+50,1.5f,cs,tc*0.9f,tc*0.85f,0.35f,1);
    }
    // pannello info / costo-linea: parte DOPO l'ultima card della tab attiva
    // (il numero di card cambia per tab e cresce col catalogo — mai fisso)
    float ix=gPrepCardR[0].x+(float)tb->n*(PREP_CARD_W+8)+12;
    if(gLineOn){                                    // drag di linea: costo live
        char li[64];
        snprintf(li,sizeof li,"LINEA %.1f M - %d$%s",(double)gLineLen,gLineCost,
                 gLineValid?"":(gHost.plc->reason==PL_NOFUNDS?" - BUDGET!":" - OCCUPATO"));
        ui_text(ix,gPrepCardR[0].y+8,2,li,
                gLineValid?0.30f:0.95f,gLineValid?0.95f:0.25f,0.25f,1);
        ui_text(mpx+16,mpy-20,2,li,gLineValid?0.30f:0.95f,gLineValid?0.95f:0.25f,0.25f,1);
    } else if(sel){                                 // pannello info (§3)
        char l1[80],l2[96];
        prep_info_lines(sel,l1,sizeof l1,l2,sizeof l2);
        ui_text(ix,gPrepCardR[0].y+8,2,l1,0.95f,0.85f,0.45f,1);
        ui_text(ix,gPrepCardR[0].y+26,2,l2,0.80f,0.80f,0.80f,1);
    }
    { const UiRect *r=&gPrepGoR;                    // VIA ALL'ORDA (=INVIO)
      ui_quad(r->x,r->y,r->w,r->h,0.50f,0.10f,0.07f,0.95f);
      ui_text_c(r->x+r->w*0.5f,r->y+6,3,"VIA ALL'ORDA",1,0.92f,0.85f,1);
      ui_text_c(r->x+r->w*0.5f,y0+100,1.5f,"INVIO",0.6f,0.6f,0.6f,1); }
}

// ---- barra ASSALTO (BIOMASS_DESIGN v2 §4): SOLO i verbi del giocatore —
// MORTAIO / RIPARA / REGOLA, ognuno una modalità esclusiva col suo costo — più
// il serbatoio di biomassa (una barra, un numero). Niente card di produzione:
// il convertitore v1 non esiste più. Layout on demand come la barra PREP.
static UiRect gMortR, gRepR, gAdjR, gSolR; static float gStrikeBarY = 1e9f;
static UiRect gBldR[BUILD_NCAT];     // card COSTRUISCI (LOOP_DESIGN C)
#define VERB_W 132.0f
static void strikes_ui_layout(int SW, int SH) {
    (void)SW;
    float y0 = (float)SH - PREP_BAR_H; gStrikeBarY = y0;
    gMortR = (UiRect){ 10.0f,               y0 + 36, VERB_W, PREP_CARD_H };
    gRepR  = (UiRect){ 10.0f + VERB_W + 8,  y0 + 36, VERB_W, PREP_CARD_H };
    gAdjR  = (UiRect){ 10.0f + 2*(VERB_W+8),y0 + 36, VERB_W, PREP_CARD_H };
    gSolR  = (UiRect){ 10.0f + 3*(VERB_W+8),y0 + 36, VERB_W, PREP_CARD_H };
    // card di costruzione: dopo i verbi, staccate da un gap (gruppo diverso)
    for (int i = 0; i < BUILD_NCAT; i++)
        gBldR[i] = (UiRect){ 10.0f + 4*(VERB_W+8) + 26 + i*(VERB_W+8),
                             y0 + 36, VERB_W, PREP_CARD_H };
}
// pulsante-verbo: cornice accesa in modalità, titolo + tasto + costo
static void verb_button(const UiRect *r, int on, int afford,
                        const char *title, const char *key, const char *cost) {
    ui_quad(r->x, r->y, r->w, r->h, on ? 0.28f : 0.12f, on ? 0.14f : 0.10f, 0.10f, 0.92f);
    ui_quad(r->x, r->y, r->w, 2, 0.65f, 0.32f, 0.12f, 1);
    if (on) {                                        // cornice della modalità attiva
        ui_quad(r->x, r->y + r->h - 2, r->w, 2, 0.95f, 0.55f, 0.20f, 1);
        ui_quad(r->x, r->y, 2, r->h, 0.95f, 0.55f, 0.20f, 1);
        ui_quad(r->x + r->w - 2, r->y, 2, r->h, 0.95f, 0.55f, 0.20f, 1);
    }
    float cx = r->x + r->w * 0.5f, c = afford ? 0.95f : 0.45f;
    ui_text_c(cx, r->y + 10, 2, title, c, c * 0.95f, c * 0.9f, 1);
    ui_text_c(cx, r->y + 30, 1.5f, cost, afford ? 0.55f : 0.40f,
              afford ? 0.95f : 0.45f, 0.45f, 1);
    ui_text_c(cx, r->y + r->h - 13, 1.5f, key, 0.70f, 0.70f, 0.70f, 1);
}
static void strikes_bar_draw(int SW, int SH) {
    strikes_ui_layout(SW, SH);
    float y0 = gStrikeBarY;
    ui_quad(0, y0, (float)SW, PREP_BAR_H, 0.05f, 0.05f, 0.09f, 0.88f);
    ui_quad(0, y0, (float)SW, 2, 0.35f, 0.10f, 0.08f, 1);
    // riga alta: IL serbatoio (§3) — un numero, una barra, il flash dello spreco
    { float tank = bio_tank(&gBio), cap = bio_cap(&gBio);
      char tt[64]; snprintf(tt, sizeof tt, "BIOMASSA %.0f/%.0f",
                            (double)tank, (double)cap);
      ui_text(10, y0 + 9, 2, tt, 0.55f, 0.95f, 0.45f, 1);
      float bw = 220.0f, bx = 10 + ui_text_w(2, tt) + 12;
      ui_quad(bx, y0 + 10, bw, 12, 0.10f, 0.14f, 0.10f, 0.9f);
      float f = cap > 0 ? tank / cap : 0.0f; if (f > 1) f = 1;
      int hot = (gBioFlash > 0.0f);                  // kill sprecato: lampeggia
      ui_quad(bx, y0 + 10, bw * f, 12, hot ? 0.95f : 0.30f,
              hot ? 0.35f : 0.80f, 0.30f, 1);
      if (bio_full(&gBio))                           // lo spreco resta INFORMATO
          ui_text(bx + bw + 16, y0 + 9, 2, "SERBATOIO PIENO - BIOMASSA SPRECATA",
                  0.95f, 0.25f, 0.20f, 1);
    }
    float tank = bio_tank(&gBio);
    // MORTAIO: in cooldown il pulsante dice quanto manca (il rifiuto del click
    // dev'essere LEGGIBILE, non sembrare un bug) + barra di raffreddamento.
    { char c[28];
      if (gMortCd > 0.0f) snprintf(c, sizeof c, "PRONTO IN %.1f S", (double)gMortCd);
      else                snprintf(c, sizeof c, "%.0f BIO", (double)BIO_MORTAR_COST);
      verb_button(&gMortR, gAimMort, gMortCd <= 0.0f && tank >= BIO_MORTAR_COST,
                  "MORTAIO", "M", c);
      if (gMortCd > 0.0f && gMortCdMax > 0.0f) {
          float f = 1.0f - gMortCd / gMortCdMax;      // 0 appena sparato -> 1 pronto
          const UiRect *r = &gMortR;
          ui_quad(r->x + 6, r->y + r->h - 24, r->w - 12, 4, 0.16f, 0.16f, 0.20f, 0.9f);
          ui_quad(r->x + 6, r->y + r->h - 24, (r->w - 12) * f, 4, 0.95f, 0.70f, 0.20f, 1);
      } }
    verb_button(&gRepR, gAimRepair, tank > 0.0f, "RIPARA", "R", "1 BIO / HP");
    verb_button(&gAdjR, gAdjOn, 1, "REGOLA", "V",
                BIO_ADJUST_COST > 0.0f ? "COSTO" : "GRATIS");
    // SOLDATO (SOLDIER_DESIGN): in lockout la card conta i secondi come il
    // mortaio in cooldown; il mitra è gratis, la granata paga a colpo.
    { char c[28]; float dn = gSol ? soldier_down(gSol) : 0.0f;
      if (dn > 0.0f) snprintf(c, sizeof c, "PRONTO IN %.1f S", (double)dn);
      else           snprintf(c, sizeof c, gSolMode ? "IN CAMPO" : "GRATIS");
      verb_button(&gSolR, gSolMode, dn <= 0.0f, "SOLDATO", "F", c); }
    // card COSTRUISCI (LOOP_DESIGN C): il circolo TD — kill -> biomassa ->
    // più difese. Prezzo bio maggiorato (wallet), barricata prezzata al metro.
    { Placement *p = gHost.plc;
      static const char *bt[BUILD_NCAT] = { "TORRETTA L", "BARRICATA" };
      static const char *bk[BUILD_NCAT] = { "L", "B" };
      for (int i = 0; i < BUILD_NCAT; i++) {
          const PlItem *it = &PL_CAT_GAME[BUILD_CAT[i]];
          int price = bio_wallet_price(0, it->cost);
          char c[24];
          snprintf(c, sizeof c, it->kind == PL_BARRICADE ? "%d BIO/M" : "%d BIO",
                   price);
          int on = p && p->active && p->sel == BUILD_CAT[i];
          verb_button(&gBldR[i], on, tank >= (float)price, bt[i], bk[i], c);
      } }
    // riga hint contestuale in fondo alla barra
    Placement *pp = gHost.plc;
    int building = pp && pp->active;
    char lhint[96]; lhint[0] = 0;
    if (building && gLineOn)             // drag di linea: costo live (§5)
        snprintf(lhint, sizeof lhint, "LINEA %.1f M - %d BIO%s",
                 (double)gLineLen, gLineCost, gLineValid ? "" : " - NON VALIDA");
    const char *hint =
        gSolMode && building ?
                     "IL SOLDATO COSTRUISCE - LMB PIAZZA DAVANTI A LUI - RMB ANNULLA" :
        gSolMode && gAdjOn ?
                     "TRASCINA DA UNA TORRETTA VICINA = IL SOLDATO LA GIRA (RMB/V ANNULLA)" :
        gSolMode   ? "WASD MUOVE - MOUSE MIRA - LMB MITRA - RMB GRANATA - F RIENTRA" :
        gAimMort   ? "MIRA COL MOUSE - CLICK SINISTRO = FUOCO (RMB/M ANNULLA)" :
        gAimRepair ? "TIENI PREMUTO SU UNA STRUTTURA = RIPARA (RMB/R ANNULLA)" :
        gAdjOn     ? "TRASCINA DA UNA TORRETTA = NUOVA DIREZIONE (RMB/V ANNULLA)" :
        lhint[0]   ? lhint :
        building   ? "PIAZZA SUL TERRENO - RMB ANNULLA" :
        "CLICK SU UNA TORRETTA IN RICARICA = RICARICA SUBITO - M MORTAIO - R RIPARA - V REGOLA";
    ui_text_c((float)SW * 0.5f, y0 + 101, 1, hint, 0.65f, 0.65f, 0.65f, 1);
}
// toggle di una card COSTRUISCI (click in barra O hotkey L/B): stessa
// semantica in entrambi i punti, soldato-costruttore incluso
static void build_card_toggle(int i) {
    Placement *p = gHost.plc;
    if (!p || i < 0 || i >= BUILD_NCAT) return;
    if (p->active && p->sel == BUILD_CAT[i]) p->active = 0;  // toggle
    else { if (sol_builder()) sol_verbs_off();  // costruisce LUI, resta
           else bio_mode_set(0, 0, 0);
           pl_select(p, BUILD_CAT[i]); p->active = 1; }
    au_play(SND_MENU_SELECT);
}
static int strikes_ui_click(float mx, float my, int SW, int SH) {
    strikes_ui_layout(SW, SH);
    if (my < gStrikeBarY) return 0;
    Placement *p = gHost.plc;
    if (ui_hit(&gMortR, mx, my)) {                  // i verbi spengono il ghost
        bio_mode_set(!gAimMort, 0, 0);
        if (gAimMort && p) p->active = 0;
        au_play(SND_MENU_SELECT); return 1; }
    if (ui_hit(&gRepR, mx, my)) {
        bio_mode_set(0, !gAimRepair, 0);
        if (gAimRepair && p) p->active = 0;
        au_play(SND_MENU_SELECT); return 1; }
    if (ui_hit(&gAdjR, mx, my)) {
        int on = !gAdjOn;
        if (sol_builder()) { sol_verbs_off(); gAdjOn = on; }  // il soldato resta
        else bio_mode_set(0, 0, on);
        if (gAdjOn && p) p->active = 0;
        au_play(SND_MENU_SELECT); return 1; }
    if (ui_hit(&gSolR, mx, my)) {   // SOLDATO: toggle servito nel frame body
        gSolWant = 1; return 1; }   // (deploy vuole s; suono all'esito, la')
    for (int i = 0; i < BUILD_NCAT; i++)            // card = seleziona E attiva
        if (p && ui_hit(&gBldR[i], mx, my)) {
            build_card_toggle(i); return 1; }
    return 1;                                       // sfondo barra: consumato (mai mondo)
}

// picking per la RIPARAZIONE (§4): struttura NON crollata piu' vicina al punto
// cliccato (scan celle nel raggio del click). Fallback dell'hover-inspect.
static int repair_pick_struct(DefGame *g, SimP *s, float wx, float wy) {
    float cs = simp_cell_size(s), R = 1.6f;
    int cx0 = (int)((wx - R) / cs), cy0 = (int)((wy - R) / cs);
    int cx1 = (int)((wx + R) / cs), cy1 = (int)((wy + R) / cs);
    int best = -1; float bd = 1e30f;
    for (int cy = cy0; cy <= cy1; cy++)
        for (int cx = cx0; cx <= cx1; cx++) {
            int sid = def_cell_struct(g, cx, cy);
            if (sid < 0 || def_struct_collapsed(g, sid)) continue;
            float dx = (cx + 0.5f) * cs - wx, dy = (cy + 0.5f) * cs - wy;
            float d = dx * dx + dy * dy;
            if (d < bd) { bd = d; best = sid; }
        }
    return best;
}
// picking per la ricarica istantanea (§5, click): torretta viva col caricatore
// NON pieno (o in ricarica) sotto il cursore.
static int reload_pick_turret(DefGame *g, float wx, float wy) {
    int nt = def_turret_count(g), best = -1; float bd = 1.4f * 1.4f;
    for (int i = 0; i < nt; i++) { DefTurret *t = def_turret(g, i);
        if (def_turret_disabled(g, i) || t->mag_size <= 0) continue;
        if (t->mag >= t->mag_size && def_turret_reloading(g, i) <= 0.0f) continue;
        float dx = t->x - wx, dy = t->y - wy, d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = i; } }
    return best;
}
// picking per REGOLA (§4): una torretta viva qualunque sotto il cursore.
static int adjust_pick_turret(DefGame *g, float wx, float wy) {
    int nt = def_turret_count(g), best = -1; float bd = 1.4f * 1.4f;
    for (int i = 0; i < nt; i++) { DefTurret *t = def_turret(g, i);
        if (def_turret_disabled(g, i)) continue;
        float dx = t->x - wx, dy = t->y - wy, d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = i; } }
    return best;
}

// --- hover inspect (decisione 2026-07-08): feedback su difese/strutture SENZA
// barre HP world-space (provate: ingombranti alla scala del gioco). Tooltip
// SCREEN-SPACE accanto al cursore, solo sull'elemento sotto il mouse: nome +
// mini-barra HP. Il cursore becca la struttura su TUTTA la sagoma visibile,
// non solo il footprint: il chiamante scandisce i piani di quota dall'alto
// verso terra (pick_ray + pick_ray_plane, un solo unproject) e a ogni piano h
// questo resolver accetta solo elementi la cui ALTEZZA raggiunge h — il primo
// hit scendendo è la faccia davanti sullo schermo. ~20 piani × lookup O(1):
// costo irrilevante, niente raycast contro le mesh.
static int gHovOn=0; static char gHovLabel[64];
static float gHovHp=0, gHovHpMax=0;     // hp_max<=0 = senza barra (indistruttibile)
static int gHovTurret=-1;               // torretta sotto il cursore -> cono a terra
static int gHovSid=-1;                  // struttura sotto il cursore (bersaglio kit)
#define HOVER_TURRET_H 1.9f             // altezza cliccabile della torretta
#define HOVER_STRUCT_H 2.8f             // muri/barriere/nucleo (H del render)
#define HOVER_SCAN_TOP 8.0f             // quota massima scandita (prop alti)
static int hover_resolve(const Scene *sc, DefGame *g, float wx, float wy, float h){
    // torrette per raggio (le distrutte sono sparite dal render: niente tooltip)
    if(h<=HOVER_TURRET_H){
        int nt=def_turret_count(g), best=-1; float bd=1.4f*1.4f;
        for(int i=0;i<nt;i++){ DefTurret *t=def_turret(g,i);
            if(def_turret_disabled(g,i)) continue;
            float dx=t->x-wx, dy=t->y-wy, d=dx*dx+dy*dy;
            if(d<bd){ bd=d; best=i; } }
        if(best>=0){ DefTurret *t=def_turret(g,best);
            { static const char *kn[4]={"leggera","pesante","lanciafiamme","acido"};
              snprintf(gHovLabel,sizeof gHovLabel,"Torretta %s",
                       kn[(t->kind>=0&&t->kind<4)?t->kind:0]); }
            gHovHp=gHovHpMax=0;                   // indistruttibile: solo il nome
            int cx=(int)(t->x/sc->cell), cy=(int)(t->y/sc->cell);
            int sid=def_cell_struct(g,cx,cy);
            if(sid>=0 && def_struct_is_turret(g,sid)){
                gHovHp=def_struct_hp(g,sid); gHovHpMax=def_struct_hp_max(g,sid);
                gHovSid=sid; }
            gHovTurret=best;
            gHovOn=1; return 1; } }
    int cx=(int)(wx/sc->cell), cy=(int)(wy/sc->cell);
    int id=def_cell_struct(g,cx,cy);
    if(id<0) return 0;
    if(def_struct_is_turret(g,id)) return 0;      // coperte dal raggio qui sopra
    float eh=HOVER_STRUCT_H;                      // altezza dell'elemento colpito
    const char *lab="Muro";
    if(id==gLzCore){ lab="Base operativa"; eh=BASE_H; }
    else if(id==gCoreId)  lab="Nucleo";
    else if(id==gOuterId) lab="Mura esterne";
    else if(id<PROP_WORLD_MAX_STRUCT && gPropW.struct_is_prop[id]){
        lab="Struttura";
        for(int i=0;i<sc->n_prop && i<gPropW.n;i++) if(gPropW.struct_id[i]==id){
            const PropDef *d=prop_catalog_find(&gCatalog,sc->prop[i].key);
            if(d){ if(d->label[0]) lab=d->label; else lab=sc->prop[i].key;
                   if(d->height>0.0f) eh=d->height; }
            else lab=sc->prop[i].key;
            break; }
    }
    else if(id<PLMOD_SIDCAP && gSidMod[id]) lab="Barriera";
    if(h>eh) return 0;                            // piano sopra la cima: si scende
    snprintf(gHovLabel,sizeof gHovLabel,"%s",lab);
    gHovHp=def_struct_hp(g,id); gHovHpMax=def_struct_hp_max(g,id);
    gHovSid=id;
    gHovOn=1; return 1;
}
// tooltip accanto al cursore (chiamata dentro shell_build_ui, spazio schermo)
static void hover_tooltip_draw(float W, float H, float mpx, float mpy){
    if(!gHovOn) return;
    float s=1.5f;
    float w=ui_text_w(s,gHovLabel); if(w<44)w=44;
    float h=(gHovHpMax>0.0f)?24.0f:15.0f;
    float tx=mpx+13, ty=mpy+11;
    if(tx+w+10>W) tx=mpx-w-16;                    // non uscire dallo schermo
    if(ty+h+6>H)  ty=mpy-h-14;
    ui_quad(tx-4,ty-3,w+8,h,0.04f,0.04f,0.08f,0.82f);
    ui_text(tx,ty,s,gHovLabel,0.95f,0.92f,0.85f,1);
    if(gHovHpMax>0.0f){
        float frac=gHovHp/gHovHpMax;
        if(frac<0)frac=0;
        if(frac>1)frac=1;
        ui_quad(tx,ty+14,w,4,0.22f,0.22f,0.26f,1);
        ui_quad(tx,ty+14,w*frac,4, 0.20f+0.70f*(1.0f-frac), 0.15f+0.75f*frac, 0.12f, 1);
    }
}

// --- barra di reload world-space (BIOMASS §5) + cursori-icona delle modalità
// (§4). La barra appare SOLO durante la ricarica e sparisce a caricatore pieno:
// il giocatore vede quanto manca e decide se pagare la ricarica istantanea.
// Il frame body proietta i punti sopra la canna, la UI li disegna in pixel.
#define REL_BAR_MAX 64
static struct { float x, y, f; } gRelBar[REL_BAR_MAX];   // schermo + progresso 0..1
static int gRelBarN = 0;
// proiezione world -> pixel (vp è column-major come lo vuole GL). 0 = dietro la camera.
static int world_to_screen(const float vp[16], float x, float y, float z,
                           int W, int H, float *sx, float *sy){
    float w = vp[3]*x + vp[7]*y + vp[11]*z + vp[15];
    if(w <= 1e-4f) return 0;
    float cx = (vp[0]*x + vp[4]*y + vp[8]*z + vp[12]) / w;
    float cy = (vp[1]*x + vp[5]*y + vp[9]*z + vp[13]) / w;
    *sx = (cx*0.5f + 0.5f) * (float)W;
    *sy = (1.0f - (cy*0.5f + 0.5f)) * (float)H;
    return 1;
}
static void reload_bars_draw(void){
    for(int i=0;i<gRelBarN;i++){
        float bw=44.0f, bh=6.0f;
        float x=gRelBar[i].x-bw*0.5f, y=gRelBar[i].y-bh*0.5f;
        float f=gRelBar[i].f; if(f<0)f=0; if(f>1)f=1;
        ui_quad(x-1,y-1,bw+2,bh+2,0.02f,0.02f,0.03f,0.75f);
        ui_quad(x,y,bw,bh,0.16f,0.16f,0.20f,0.9f);
        ui_quad(x,y,bw*f,bh,0.95f,0.70f,0.20f,1);        // ambra: torretta MUTA
    }
    // HP del soldato (SOLDIER_DESIGN): transitoria come le barre di reload —
    // esiste solo in modalità, verde che vira al rosso sotto il 40%.
    if(gSolBarOn){
        float bw=44.0f, bh=6.0f;
        float x=gSolBarX-bw*0.5f, y=gSolBarY-bh*0.5f;
        float f=gSolBarF; if(f<0)f=0; if(f>1)f=1;
        ui_quad(x-1,y-1,bw+2,bh+2,0.02f,0.02f,0.03f,0.75f);
        ui_quad(x,y,bw,bh,0.16f,0.16f,0.20f,0.9f);
        ui_quad(x,y,bw*f,bh, f>0.4f?0.30f:0.85f, f>0.4f?0.80f:0.25f, 0.25f,1);
    }
}
// icona accanto al cursore: dice in che modalità si è senza guardare la barra
// (§4: quad/glifi del layer UI, niente pipeline asset né SDL cursor).
static void mode_cursor_draw(float mpx, float mpy){
    float x=mpx+14, y=mpy+14;
    if(gAimRepair){                                       // martello
        ui_quad(x,y+8,4,10,0.75f,0.60f,0.35f,1);          // manico
        ui_quad(x-4,y+2,14,6,0.80f,0.82f,0.86f,1);        // testa
    } else if(gAdjOn){                                    // V (evoca il cono)
        for(int k=0;k<6;k++){
            ui_quad(x+(float)k,      y+2+(float)k*2, 3,3, 0.95f,0.75f,0.25f,1);
            ui_quad(x+12-(float)k,   y+2+(float)k*2, 3,3, 0.95f,0.75f,0.25f,1);
        }
    }
    // il MORTAIO ha già la sua X di mira sul mondo: niente icona al cursore
}

// riparazione a MANTENIMENTO (BIOMASS §4): finché LMB resta giù su una struttura
// danneggiata la biomassa cola in HP a BIO_REPAIR_RATE (1 bio = 1 HP). Si ferma
// da sola a hp_max, a serbatoio vuoto o al rilascio. Mai sulle crollate (il
// crollo è definitivo). Bersaglio = quello dell'hover (piani di quota: prende la
// torretta su tutta la sagoma). Chiamata dal frame body, non dagli eventi: la
// spesa è un FLUSSO, deve essere dosabile.
static void repair_channel(DefGame *g, float dt){
    if(!gAimRepair || !gRepairHold) return;
    int sid=gHovSid;
    if(sid<0 || def_struct_collapsed(g,sid)) return;
    float miss=def_struct_hp_max(g,sid)-def_struct_hp(g,sid);
    if(miss<=0.0f) return;                       // già sana: niente spesa
    float want=BIO_REPAIR_RATE*dt;
    if(want>miss) want=miss;
    if(want>bio_tank(&gBio)) want=bio_tank(&gBio);
    if(want<=0.0f) return;                       // serbatoio a secco
    bio_take(&gBio,want);
    def_struct_repair(g,sid,want);               // 1 bio = 1 HP
}

// gesto REGOLA (BIOMASS §4), condiviso PREP/ASSALTO: LMB su una torretta prende
// il suo cono, il drag lo orienta (gAdjFacing lo aggiorna il frame body dal
// cursore), il rilascio committa il nuovo facing pagandone il costo; RMB annulla
// il gesto in corso, o esce dalla modalità. Stesso gesto del piazzamento
// direzionale.
static void adjust_event(const SDL_Event *e, DefGame *g, const float *vp,
                         float mxf, float myf, int SW, int SH){
    if(e->type==SDL_EVENT_MOUSE_BUTTON_DOWN){
        if(e->button.button==SDL_BUTTON_LEFT){
            int tid=gHovTurret; float wx,wy;
            if(tid<0 && pick_y0(vp,mxf,myf,SW,SH,&wx,&wy))
                tid=adjust_pick_turret(g,wx,wy);
            if(tid>=0 && sol_builder()){   // in campo la gira il soldato:
                DefTurret *t=def_turret(g,tid);          // serve arrivarci
                float dx=t->x-soldier_x(gSol), dy=t->y-soldier_y(gSol);
                if(dx*dx+dy*dy>SOL_ADJUST_REACH*SOL_ADJUST_REACH) tid=-1;
            }
            if(tid>=0){ gAdjTid=tid; gAdjFacing=def_turret(g,tid)->ang;
                        au_play(SND_MENU_MOVE); }
            else au_play(SND_MENU_MOVE);
        } else if(e->button.button==SDL_BUTTON_RIGHT){
            if(gAdjTid>=0) gAdjTid=-1; else gAdjOn=0;
        }
    } else if(e->type==SDL_EVENT_MOUSE_BUTTON_UP &&
              e->button.button==SDL_BUTTON_LEFT && gAdjTid>=0){
        if(bio_take(&gBio,BIO_ADJUST_COST)){
            def_turret_set_facing(g,gAdjTid,gAdjFacing);
            au_play(SND_MENU_SELECT);
        } else au_play(SND_MENU_MOVE);           // costo attivo e serbatoio corto
        gAdjTid=-1;
    }
}

// pulsanti del menu: colonna sul lato DESTRO del title screen (scelta utente
// 2026-07-12). Layout condiviso da render e hit test del mouse (mai rect
// stantii, stesso principio della barra PREP §4).
static void menu_btn_rect(int i,float W,float H,UiRect *r){
    float bw=W*0.22f;
    float bh=(gUiTex.bw>0)?bw*(float)gUiTex.bh/(float)gUiTex.bw:bw*0.28f;
    if(bh>H*0.11f){ float k=H*0.11f/bh; bh*=k; bw*=k; }   // clamp, aspect intatto
    float gap=bh*0.30f;
    float tot=APP_MENU_COUNT*bh+(APP_MENU_COUNT-1)*gap;
    r->x=W*0.78f-bw*0.5f;
    r->y=H*0.52f-tot*0.5f+(float)i*(bh+gap);
    r->w=bw; r->h=bh;
}
static void title_bg_draw(float W,float H){    // titlescreen a copertura (crop)
    if(!gUiTex.title){ ui_quad(0,0,W,H,0.02f,0.02f,0.04f,0.92f); return; }
    float sc=W/(float)gUiTex.tw;
    if(sc*(float)gUiTex.th<H) sc=H/(float)gUiTex.th;
    float iw=(float)gUiTex.tw*sc, ih=(float)gUiTex.th*sc;
    ui_img(gUiTex.title,(W-iw)*0.5f,(H-ih)*0.5f,iw,ih,1,1,1,1);
}
static void shell_build_ui(int SW,int SH,DefGame *g,float mpx,float mpy){
    float W=(float)SW,H=(float)SH;
    AppState st=gApp.state;
    const AppLevel *L=app_cur_level(&gApp);
    if(st==APP_PREP||st==APP_ASSAULT){                  // barra di fase in alto
        char line[192];
        if(st==APP_PREP)
            snprintf(line,sizeof line,"PREPARAZIONE - %s | PIAZZA LE DIFESE, "
                     "POI VIA ALL'ORDA",L?L->name:"?");
        else {
            // fase A: se la scena dichiara la missione comanda mission.c —
            // il timer di AppLevel resta solo per le arene legacy.
            float left = gMissionOn ? mission_time_left(&gMission)
                       : (L ? L->survive_s-gSurviveT : 0.0f);
            if(left<0)left=0;
            if(gMissionOn && gMission.kind==SCENE_MISSION_CLEAR)
                snprintf(line,sizeof line,"ASSALTO - ELIMINA L'ORDA | KILLS %d",
                         def_kills(g));
            else
                snprintf(line,sizeof line,"ASSALTO - RESISTI ANCORA %d S | KILLS %d%s",
                         (int)(left+0.5f),def_kills(g),
                         (gShellCore>=0&&def_struct_hp(g,gShellCore)<def_struct_hp_max(g,gShellCore))
                         ?" | IL NUCLEO E' SOTTO ATTACCO":"");
            // LOOP A: contatore ondate nella riga di fase (k/N; N+1 = mop-up)
            if(gMissionOn && mission_wave_total(&gMission)>0){
                int wc=mission_wave_current(&gMission), wt=mission_wave_total(&gMission);
                char wl[32];
                if(wc>wt) snprintf(wl,sizeof wl," | ONDATE FINITE");
                else      snprintf(wl,sizeof wl," | ONDATA %d/%d",wc,wt);
                strncat(line,wl,sizeof line-strlen(line)-1);
            } }
        ui_quad(0,0,W,28,0,0,0,0.55f);
        ui_text(10,8,2,line,1,1,1,1);
        // LOOP A: banner d'annuncio (pausa fra ondate = finestra decisionale)
        // o, subito dopo la chiamata, il flash del bonus biomassa.
        if(st==APP_ASSAULT){
            char wb[160];
            if(wave_banner(gHost.sc,wb,sizeof wb)){
                float s=2.6f,w=ui_text_w(s,wb);
                float pulse=0.72f+0.28f*sinf(gRouteT*5.0f);
                ui_quad((W-w)*0.5f-12,36,w+24,s*FONT8_H+14,0.32f,0.03f,0.03f,0.78f);
                ui_text_c(W*0.5f,43,s,wb,1.0f,0.55f+0.35f*pulse,0.25f,1);
            } else if(gWaveMsgT>0.0f){
                float s=2.6f,w=ui_text_w(s,gWaveMsg);
                ui_quad((W-w)*0.5f-12,36,w+24,s*FONT8_H+14,0.02f,0.22f,0.05f,0.70f);
                ui_text_c(W*0.5f,43,s,gWaveMsg,0.45f,1.0f,0.45f,1);
            }
        }
        if(st==APP_PREP) prep_bar_draw(SW,SH,g,mpx,mpy);   // barra PREP (§3)
        else if(st==APP_ASSAULT) strikes_bar_draw(SW,SH);  // barra verbi (BIOMASS §4)
        reload_bars_draw();                                // torrette in ricarica (§5)
        hover_tooltip_draw(W,H,mpx,mpy);                   // ispezione al cursore
        mode_cursor_draw(mpx,mpy);                         // icona della modalità (§4)
        return;
    }
    if(st==APP_DEPLOY||st==APP_EXTRACT){                // cinematiche: mondo visibile
        ui_quad(0,0,W,28,0,0,0,0.55f);
        ui_text(10,8,2, st==APP_DEPLOY
                ?"CONSEGNA DELLA BASE IN CORSO - INVIO PER SALTARE"
                :"MISSIONE COMPIUTA - ESTRAZIONE DEL CONTAINER - INVIO PER SALTARE",
                1,1,1,1);
        return;
    }
    // scala UI delle schermate a pieno schermo (title/menu/settings/briefing/
    // debrief): le taglie storiche sono tarate su 720p, in fullscreen il testo
    // segue la finestra (i pulsanti/posizioni sono già proporzionali a W/H).
    // Le barre HUD di gioco (PREP/ASSALTO, cinematiche) restano in pixel.
    float uk=H/720.0f;
    if(st==APP_TITLE){
        title_bg_draw(W,H);
        if(!gUiTex.title){        // fallback senza immagine: il vecchio titolo testo
            ui_text_c(W*0.5f,H*0.26f,12*uk,"HORDE",0.85f,0.15f,0.10f,1);
            ui_text_c(W*0.5f,H*0.26f+12*uk*(FONT8_H+3),3*uk,"SIGNORE, ABBIAMO UN PROBLEMA DI ZOMBIE",
                      0.75f,0.75f,0.75f,1);
        }
        ui_text_cb(W*0.5f,H*0.88f,3*uk,"PREMI UN TASTO",1,1,1,0.9f);
        return;
    }
    if(st==APP_MENU){
        title_bg_draw(W,H);
        if(!gUiTex.title) ui_text_c(W*0.5f,H*0.12f,8*uk,"HORDE",0.85f,0.15f,0.10f,1);
        if(gApp.campaign_done)
            ui_text_cb(W*0.5f,H*0.08f,3*uk,"CAMPAGNA COMPLETATA!",0.3f,0.9f,0.3f,1);
        for(int i=0;i<APP_MENU_COUNT;i++){
            const char *lab=app_menu_label(i);
            int en=app_menu_enabled(&gApp,i),sel=(i==gApp.menu_idx);
            UiRect r; menu_btn_rect(i,W,H,&r);
            if(gUiTex.btn){
                // hover: pulsante_hover.png se c'è, altrimenti la stessa
                // texture schiarita; disabilitato = spento
                GLuint tex=(sel&&gUiTex.btn_hov)?gUiTex.btn_hov:gUiTex.btn;
                float lum=en?((sel&&!gUiTex.btn_hov)?1.35f:1.0f):0.45f;
                ui_img(tex,r.x,r.y,r.w,r.h,lum,lum,lum,1);
            } else
                ui_quad(r.x,r.y,r.w,r.h,sel?0.45f:0.16f,0.08f,0.06f,0.85f);
            // etichetta agganciata all'ALTEZZA del pulsante (cap ~42%):
            // scala con la finestra insieme al pulsante stesso
            float s=r.h*0.42f/FONT8_H,c=en?1.0f:0.5f;
            ui_text_cb(r.x+r.w*0.5f,r.y+(r.h-s*FONT8_H)*0.5f,s,lab,c,c,c,1);
        }
        { UiRect r; menu_btn_rect(APP_MENU_COUNT-1,W,H,&r);
          ui_text_c(r.x+r.w*0.5f,r.y+r.h+14*uk,2*uk,"FRECCE O MOUSE = SCEGLI   INVIO O CLICK = CONFERMA",
                    0.75f,0.75f,0.75f,0.9f); }
        return;
    }
    ui_quad(0,0,W,H,0.02f,0.02f,0.04f,0.72f);           // oscura il mondo fermo
    if(st==APP_SETTINGS){
        ui_text_c(W*0.5f,H*0.16f,6*uk,"SETTINGS",0.9f,0.9f,0.9f,1);
        float s=3.5f*uk,lh=s*(FONT8_H+6),y=H*0.38f;
        for(int i=0;i<APP_SET_COUNT;i++){
            char row[64];
            if(i==APP_SET_SFX)        snprintf(row,sizeof row,"VOLUME SFX      < %2d >",gApp.vol_sfx);
            else if(i==APP_SET_MUSIC) snprintf(row,sizeof row,"VOLUME MUSICA   < %2d >",gApp.vol_music);
            else if(i==APP_SET_FULLSCREEN)
                                      snprintf(row,sizeof row,"SCHERMO INTERO  < %s >",gApp.fullscreen?"SI":"NO");
            else                      snprintf(row,sizeof row,"INDIETRO");
            int sel=(i==gApp.set_idx);
            float w=ui_text_w(s,row);
            if(sel) ui_quad(W*0.5f-w*0.5f-14*uk,y-7*uk,w+28*uk,s*FONT8_H+14*uk,0.45f,0.08f,0.06f,0.85f);
            ui_text_c(W*0.5f,y,s,row,1,1,1,1);
            y+=lh;
        }
        if(!au_backend_live())
            ui_text_c(W*0.5f,y+lh*0.5f,2*uk,"AUDIO MUTO: MANCA VAT/MINIAUDIO.H (VEDI GAME_APP_DESIGN.MD)",
                      0.7f,0.6f,0.3f,1);
    } else if(st==APP_BRIEFING){
        char hdr[96];
        snprintf(hdr,sizeof hdr,"MISSIONE %d: %s",gApp.cur+1,(L&&L->name[0])?L->name:"(SENZA NOME)");
        ui_text(W*0.14f,H*0.16f,4*uk,hdr,0.95f,0.78f,0.30f,1);
        ui_text(W*0.14f,H*0.28f,2.5f*uk,L?L->brief:"",0.92f,0.92f,0.92f,1);
        char obj[80];
        if(gMissionOn && gMission.kind==SCENE_MISSION_CLEAR)
            snprintf(obj,sizeof obj,"OBIETTIVO: ELIMINA L'ORDA%s",
                     gHost.sc->has_lz?" - DIFENDI LA LZ":"");
        else if(gMissionOn)
            snprintf(obj,sizeof obj,"OBIETTIVO: RESISTI %d SECONDI%s",
                     (int)gMission.survive_s,
                     gHost.sc->has_lz?" - DIFENDI LA LZ":"");
        else
            snprintf(obj,sizeof obj,"OBIETTIVO: RESISTI %d SECONDI%s",
                     (int)(L?L->survive_s:0),(L&&L->core_hp>0)?" - DIFENDI IL NUCLEO":"");
        ui_text(W*0.14f,H*0.60f,3*uk,obj,0.75f,0.88f,1.0f,1);
        ui_text_c(W*0.5f,H*0.80f,3*uk,
                  gApp.level_ready?"INVIO PER INIZIARE":"CARICAMENTO...",1,1,1,0.9f);
    } else if(st==APP_DEBRIEF){
        const char *T=gApp.won?"MISSIONE COMPIUTA":"POSTAZIONE PERSA";
        ui_text_c(W*0.5f,H*0.32f,6*uk,T,gApp.won?0.30f:0.90f,gApp.won?0.90f:0.20f,0.20f,1);
        char k[64]; snprintf(k,sizeof k,"KILLS %d",def_kills(g));
        ui_text_c(W*0.5f,H*0.50f,3*uk,k,0.9f,0.9f,0.9f,1);
        const char *P=gApp.won?((gApp.cur+1>=gApp.nlevels)?"INVIO PER IL MENU"
                                                          :"INVIO: PROSSIMO LIVELLO")
                              :"INVIO: RIPROVA";
        ui_text_c(W*0.5f,H*0.66f,3*uk,P,1,1,1,0.9f);
    }
}
#endif /* GAME_SHELL */

int main(int argc, char **argv){
#ifdef GAME_SHELL
    // eseguibile del gioco: argv[1] = campagna; la scena iniziale è il livello
    // del progresso salvato, mostrata ferma dietro al title screen.
    app_init(&gApp);
    const char *camp_path = argc > 1 ? argv[1] : "campaign.txt";
    if(app_campaign_load(&gApp,camp_path)!=0){
        fprintf(stderr,"campagna non caricata: %s\n",camp_path); return 1; }
    app_load_progress(&gApp,"progress.txt");
    gApp.cur = gApp.unlocked;
    const char *scene_path = app_cur_level(&gApp)->scene;
    if(au_init()!=0) fprintf(stderr,"audio init fail (si continua muti)\n");
    au_set_volume((float)gApp.vol_sfx/10.0f,(float)gApp.vol_music/10.0f);
    printf("campagna %s: %d livelli | progresso: livello %d | audio: %s\n",
           camp_path,gApp.nlevels,gApp.unlocked+1,
           au_backend_live()?"miniaudio":"muto (vat/miniaudio.h assente)");
#else
    const char *scene_path = argc > 1 ? argv[1] : "assets/scenes/obstacles.scn";
    if(au_init()!=0) fprintf(stderr,"audio init fail (si continua muti)\n");
    printf("audio: %s\n", au_backend_live()?"miniaudio":"muto (vat/miniaudio.h assente)");
#endif
    Scene sc;
    if (scene_load(scene_path, &sc) != 0) { fprintf(stderr, "scene load fail: %s\n", scene_path); return 1; }
    printf("scene %s: %dx%d cell=%g (%gx%g m) poly=%d spawn=%d goal=%d prop=%d\n",
           scene_path, sc.gw, sc.gh, (double)sc.cell, (double)sc.world_w, (double)sc.world_h,
           sc.n_poly, sc.n_spawn, sc.n_goal, sc.n_prop);

    // catalogo prop di decoro (§10 stadio 5b): tipo->mesh+scala+label. Render-only.
    const char *cpath = getenv("VAT_HORDE_PROPS") ? getenv("VAT_HORDE_PROPS") : "assets/props/catalog.txt";
    gCatN = prop_catalog_load(cpath, &gCatalog);
    if (gCatN > 0) printf("prop catalog: %s (%d tipi)\n", cpath, gCatN);
    else { gCatN = 0; printf("prop catalog: %s assente -> tool prop disabilitato\n", cpath); }
    load_prop_models(&gCatalog);   // colonna mesh -> glb (fallback placeholder)

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
    if (sc.n_spawn == 0 && sc.n_exit == 0 && !fillN) {
        fprintf(stderr, "scene senza spawn rect ne' exit\n"); return 1; }

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
#ifdef GAME_SHELL
    // opzione SCHERMO INTERO persistita (progress.txt): applicata all'avvio.
    // Mai in headless: gli screenshot restano 1280x720 deterministici.
    if(gApp.fullscreen && !shot && !bench_meas){
        fullscreen=1; SDL_SetWindowFullscreen(win,1); }
#endif
    SDL_GLContext ctx=SDL_GL_CreateContext(win);
    if(!ctx||!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)){fprintf(stderr,"GL init fail\n");return 1;}
    printf("GL %s\n",glGetString(GL_VERSION));

    GLuint prog=vg_shader("assets/shaders/vat.vs","assets/shaders/vat.fs");
    // instance buffer condiviso: ri-riempito per variante prima di ogni draw.
    GLuint bi; glGenBuffers(1,&bi);glBindBuffer(GL_ARRAY_BUFFER,bi);
    glBufferData(GL_ARRAY_BUFFER,sizeof(inst),NULL,GL_DYNAMIC_DRAW);

    // --- terreno glb (render-only): carica la mesh del suolo + shader texturizzato.
    Ground gnd; int groundOn=0;
    GLuint progGnd=0; GLint uVPgnd=0,uHasGnd=0,uColGnd=0;
    if(terrain_glb[0] && load_ground_glb(terrain_glb,&gnd)==0){ groundOn=1;
        progGnd=vg_shader("assets/shaders/ground.vs","assets/shaders/ground.fs");
        uVPgnd=glGetUniformLocation(progGnd,"uVP");
        uHasGnd=glGetUniformLocation(progGnd,"uHasTex");
        uColGnd=glGetUniformLocation(progGnd,"uColor");
        glUniform1i(glGetUniformLocation(progGnd,"uTex"),0); }
    // glb#2 statici: stessa mesh/shader del terreno, disegnato sempre. Se manca
    // il terreno (no progGnd) compiliamo comunque lo shader per disegnarlo.
    Ground gStat; int staticsOn=0;
    if(statics_glb[0] && load_ground_glb(statics_glb,&gStat)==0){ staticsOn=1;
        if(!progGnd){ progGnd=vg_shader("assets/shaders/ground.vs","assets/shaders/ground.fs");
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
    GLuint progSh=vg_shader("assets/shaders/shadow.vs","assets/shaders/shadow.fs");
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
    static float scorchinst[SCORCH_MAX*7];       // crateri scorch (§7 v1), stessa geom/shader
    GLuint progDc=vg_shader("assets/shaders/decal.vs","assets/shaders/decal.fs");
    GLint uVPdc=glGetUniformLocation(progDc,"uVP");
    GLuint dcVao,dcInst; glGenVertexArrays(1,&dcVao);glBindVertexArray(dcVao);
    glBindBuffer(GL_ARRAY_BUFFER,shDisc);        // condivide la geom del disco
    glVertexAttribPointer(0,2,GL_FLOAT,0,2*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glGenBuffers(1,&dcInst);glBindBuffer(GL_ARRAY_BUFFER,dcInst);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)DECALMAX*7*sizeof(float),NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(1,4,GL_FLOAT,0,7*sizeof(float),(void*)0);glEnableVertexAttribArray(1);glVertexAttribDivisor(1,1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,7*sizeof(float),(void*)(4*sizeof(float)));glEnableVertexAttribArray(2);glVertexAttribDivisor(2,1);
    glBindVertexArray(0);

    // --- sagome-cadavere = RING IMPOSTOR (CORPSE_DESIGN §10.8, VALIDATO
    // 2026-07-12; sostituisce il decal piatto top-down): quad billboard nel piano
    // di vista, atlante di RINGVIEWS viste azimutali bakate all'elevazione di
    // gioco (bake init-time più sotto). Persistenti come i decal di sangue.
#define CORPSEDECMAX 4096
#define CORPSE_HALF  1.15f                // semi-lato ortho del bake (m): un corpo disteso
#define RINGVIEWS    16                   // NON scendere: validato a occhio a 16
#define RING_CELL    64                   // px per cella (risparmio qui, non sulle viste)
#define RING_LOOKY   0.35f                // look-at sopra i piedi: centra il corpo nella cella
#define RING_EL      0.40f                // elevazione di bake = GAME_CAM_EL (che è
                                          // sotto #ifdef GAME_SHELL: tenerli allineati)
    static float cdecraw[CORPSEDECMAX*6];  // x,y,hd,size,colonna,outfit (da vat_layer)
    static float cdecinst[CORPSEDECMAX*7]; // cx,gy,cz,hd, half,colonna,outfit (istanza GL)
    static const float cquad[12]={-1,-1, 1,-1, 1,1, -1,-1, 1,1, -1,1};
    GLuint cdProg=vg_shader("assets/shaders/corpse_imp.vs","assets/shaders/corpse_imp.fs");
    GLint uVPcd=glGetUniformLocation(cdProg,"uVP"), uNColsCd=glGetUniformLocation(cdProg,"uNCols"),
          uRightCd=glGetUniformLocation(cdProg,"uRight"), uUp2Cd=glGetUniformLocation(cdProg,"uUp2"),
          uToCamCd=glGetUniformLocation(cdProg,"uToCam"), uLookYCd=glGetUniformLocation(cdProg,"uLookY"),
          uCamAzCd=glGetUniformLocation(cdProg,"uCamAz"), uNViewCd=glGetUniformLocation(cdProg,"uNView"),
          uNOutCd=glGetUniformLocation(cdProg,"uNOut"), uBlendCd=glGetUniformLocation(cdProg,"uBlendViews");
    GLuint cdVao,cdQuad,cdInst; glGenVertexArrays(1,&cdVao);glBindVertexArray(cdVao);
    glGenBuffers(1,&cdQuad);glBindBuffer(GL_ARRAY_BUFFER,cdQuad);
    glBufferData(GL_ARRAY_BUFFER,sizeof cquad,cquad,GL_STATIC_DRAW);
    glVertexAttribPointer(0,2,GL_FLOAT,0,2*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glGenBuffers(1,&cdInst);glBindBuffer(GL_ARRAY_BUFFER,cdInst);
    glBufferData(GL_ARRAY_BUFFER,sizeof cdecinst,NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(1,4,GL_FLOAT,0,7*sizeof(float),(void*)0);glEnableVertexAttribArray(1);glVertexAttribDivisor(1,1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,7*sizeof(float),(void*)(4*sizeof(float)));glEnableVertexAttribArray(2);glVertexAttribDivisor(2,1);
    glBindVertexArray(0);
    GLuint corpseAlb=0,corpseNrm=0;            // anello: albedo (col × outfit·vista) + normale (col × vista)
    int corpseNCols=NVAR*VAT_CORPSE_NPOSE, corpseNOut=VAT_CORPSE_NOUTFIT;
    // blocchi outfit dell'anello ALBEDO che stanno nel limite hardware (a cella 64
    // servono 16·16·64 = 16384 px di altezza = il massimo tipico); se clampa, lo
    // shader satura l'outfit all'ultimo blocco (degrado grazioso su GPU piccole).
    int nOutRing=corpseNOut;
    { GLint mt=0; glGetIntegerv(GL_MAX_TEXTURE_SIZE,&mt);
      int maxBlk=(int)mt/(RING_CELL*RINGVIEWS); if(maxBlk<1)maxBlk=1;
      if(nOutRing>maxBlk){ nOutRing=maxBlk;
          fprintf(stderr,"ring cadaveri: outfit clampati a %d (GL_MAX_TEXTURE_SIZE %d)\n",nOutRing,mt); } }

    // --- ostacoli: programma flat + mesh statica estrusa dalla scena (il quad
    // suolo flat si salta quando c'è il terreno glb).
    GLuint progFlat=vg_shader("assets/shaders/flat.vs","assets/shaders/flat.fs");
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
    // shader. Render-only (play+edit), ricostruita su ogni edit dei prop e a
    // ogni step durante i topple → store fisso DYNAMIC_DRAW + glBufferSubData.
    GLuint prVao,prVbo; glGenVertexArrays(1,&prVao);glBindVertexArray(prVao);
    glGenBuffers(1,&prVbo);glBindBuffer(GL_ARRAY_BUFFER,prVbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)PROP_MESH_CAP*9*sizeof(float),NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,9*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,0,9*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,9*sizeof(float),(void*)(6*sizeof(float)));glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    int prNV=upload_prop_mesh(prVbo,&sc,&gCatalog,&dz,g);
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

    // piazzamento a runtime (PLACEMENT_DESIGN.md): P attiva, mouse piazza in PLAY.
    // GAME_SHELL: catalogo v1 di gioco (PREP_UI_DESIGN §2) + undo di PREP (§6);
    // la sandbox tiene il catalogo pieno (bin/auto) e nessuna barra.
    Placement plc;
#ifdef GAME_SHELL
    pl_init(&plc, PL_CAT_GAME, PL_NCAT_GAME);
    static PlUndo plUndo; pl_set_undo(&plc, &plUndo);
    int plineOn=0; float plineAx=0.0f, plineAy=0.0f;   // drag di linea (§5)
    int plineChain=0;   // polilinea: SHIFT al rilascio -> vertici concatenati (click-to-place)
#else
    pl_init(&plc, PL_CAT, PL_NCAT);
#endif
    pl_set_blocked_cb(&plc, pl_blocked_host, NULL);
    // trappole (GAME_PLAN fase D): la tabella mine, alimentata dal commit PL_TRAP
    // e drenata da traps_update nel loop (mappata su host_blast).
    Traps traps; traps_init(&traps); pl_set_traps(&plc, &traps);

    // torrette (DINAMICO: una distruttibile sparisce al collasso) + tracer di
    // fuoco, stesso flat shader. Buffer riusato ogni frame da build_turret_mesh.
    // modelli torretta (base + gun) — glb con nodi "base"/"gun". Caricati PRIMA di
    // dimensionare il buffer (turVpe legge base.nv+gun.nv). Solo parsing CPU, no GL.
    { const char *ts=getenv("VAT_HORDE_TURRET_SCALE"); if(ts) gTurScale=atof(ts); }
    { const char *bv=getenv("VAT_HORDE_BULLET_V"); if(bv) gBulletV=atof(bv); }
    load_turret_model("assets/models/light_turret.glb", &gTurM[0]);
    load_turret_model("assets/models/heavy_turret.glb", &gTurM[1]);
    load_turret_model("assets/models/flame_turret.glb", &gTurM[2]);
    load_turret_model("assets/models/acid_turret.glb",  &gTurM[3]);
    load_glb_soup("assets/models/destroyed_turret.glb","torretta distrutta",
                  gTurScale,&gTurWreckM);   // rottame post-crollo (fallback moncone)
    // base container + mortaio (BASE_DESIGN §3/§5): solo parsing CPU, il
    // rendering passa dal buffer strutture (build_struct_mesh).
    load_base_model("assets/models/base_and_mortar.glb", &gBaseM);
    // mortaio/caricatori: balance.cfg + env, applicati in host_apply_balance
    // (dentro build_world, così il rebuild ricarica il file).
    printf("torrette 3D: light=%s heavy=%s flame=%s acid=%s (scala %.2f)\n",
           gTurM[0].ok?"ok":"pilastrino", gTurM[1].ok?"ok":"pilastrino",
           gTurM[2].ok?"ok":"pilastrino", gTurM[3].ok?"ok":"pilastrino",
           (double)gTurScale);
    int turCap=def_turret_count(g); if(turCap<NT) turCap=NT;   // >= tracer's NT cap
    // verts/torretta: pilastrino=30, oppure la parte più grossa di un modello glb
    // (il rottame post-crollo conta anche lui: box fallback = 36)
    int turVpe=36;
    for(int m=0;m<4;m++) if(gTurM[m].ok){ int v=gTurM[m].base.nv+gTurM[m].gun.nv; if(v>turVpe)turVpe=v; }
    if(gTurWreckM.nv>turVpe) turVpe=gTurWreckM.nv;
    int turMaxV=turCap*turVpe;
    float *turBuf=malloc((size_t)turMaxV*9*sizeof(float));
    GLuint turVao,turVbo; glGenVertexArrays(1,&turVao);glBindVertexArray(turVao);
    glGenBuffers(1,&turVbo);glBindBuffer(GL_ARRAY_BUFFER,turVbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)turMaxV*9*sizeof(float),NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,9*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,0,9*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,9*sizeof(float),(void*)(6*sizeof(float)));glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    // mine (GAME_PLAN fase D): un modello landmine.glb stampato a terra per ogni
    // trappola viva, stesso flat shader. Buffer riusato ogni frame da build_mine_mesh.
    { const char *ms=getenv("VAT_HORDE_MINE_SCALE"); if(ms) gMineScale=atof(ms); }
    int mineOk=load_glb_soup("assets/models/landmine.glb","mina",gMineScale,&gMineM);
    printf("mina 3D: %s (scala %.2f)\n", mineOk?"ok":"placeholder", (double)gMineScale);
    // soldato skinned (SOLDIER_DESIGN): glb con scheletro + clip, shader
    // skinned.*. Se manca il file o lo shader resta la sagoma verde.
#ifdef GAME_SHELL
    gSkinProg=vg_shader("assets/shaders/skinned.vs","assets/shaders/skinned.fs");
    if(gSkinProg && model_load(&gSolMdl,"assets/models/soldier.glb")){
        gSolMdlOk=1;
        anim_state_init(&gSolAnim);
        { const char *ni[]={"idle","Idle","IDLE"};
          const char *nr[]={"run","Run","walk","Walk"};
          gSolClipIdle=sol_pick_clip(&gSolMdl,ni,3);
          gSolClipRun =sol_pick_clip(&gSolMdl,nr,4);
          gSolClipBack =model_find_clip(&gSolMdl,"run_back");
          gSolClipLeft =model_find_clip(&gSolMdl,"run_left");
          gSolClipRight=model_find_clip(&gSolMdl,"run_right");
          gSolClipFL  =model_find_clip(&gSolMdl,"run_fl");
          gSolClipFR  =model_find_clip(&gSolMdl,"run_fr");
          gSolClipBL  =model_find_clip(&gSolMdl,"run_bl");
          gSolClipBR  =model_find_clip(&gSolMdl,"run_br");
          gSolClipFire=model_find_clip(&gSolMdl,"fire");
          gSolClipToss=model_find_clip(&gSolMdl,"toss");
          gSolClipClimb=model_find_clip(&gSolMdl,"climb");
          gSolClipJump =model_find_clip(&gSolMdl,"jump_down");
          // mitra in mano: bone d'aggancio (nome esatto, poi qualsiasi
          // suffisso RightHand: i rig Mixamo re-esportati cambiano prefisso)
          gSolHandBone=model_find_bone(&gSolMdl,"mixamorig:RightHand");
          if(gSolHandBone<0)
              for(int b=0;b<gSolMdl.skeleton.bone_count;b++)
                  if(strstr(gSolMdl.skeleton.bones[b].name,"RightHand")){
                      gSolHandBone=b; break; }
          gSolHandBoneL=model_find_bone(&gSolMdl,"mixamorig:LeftHand");
          if(gSolHandBoneL<0)
              for(int b=0;b<gSolMdl.skeleton.bone_count;b++)
                  if(strstr(gSolMdl.skeleton.bones[b].name,"LeftHand")){
                      gSolHandBoneL=b; break; }
          if(gSolHandBone>=0) build_gun_mesh(); }
        if(gSolClipIdle<0) gSolClipIdle=0;
        if(gSolClipRun<0)  gSolClipRun=gSolClipIdle;
        if(gSolClipBack<0)  gSolClipBack=gSolClipRun;   // glb vecchi: degrada
        if(gSolClipLeft<0)  gSolClipLeft=gSolClipRun;
        if(gSolClipRight<0) gSolClipRight=gSolClipRun;
        if(gSolClipFL<0) gSolClipFL=gSolClipRun;        // diagonali -> cardinali
        if(gSolClipFR<0) gSolClipFR=gSolClipRun;
        if(gSolClipBL<0) gSolClipBL=gSolClipBack;
        if(gSolClipBR<0) gSolClipBR=gSolClipBack;
        // fire/toss senza fallback: -1 = resta idle/gambe (niente pantomima)
        float solH=1.80f;
        { const char *e=getenv("VAT_HORDE_SOL_H"); if(e) solH=atof(e); }
        float bh=gSolMdl.bbox_max[1]-gSolMdl.bbox_min[1];
        gSolMdlScale = bh>1e-3f ? solH/bh : 1.0f;
        gSolMdlOff[0]=-0.5f*(gSolMdl.bbox_min[0]+gSolMdl.bbox_max[0]);
        gSolMdlOff[1]=-gSolMdl.bbox_min[1];
        gSolMdlOff[2]=-0.5f*(gSolMdl.bbox_min[2]+gSolMdl.bbox_max[2]);
        { const char *e=getenv("VAT_HORDE_SOL_YAW");
          if(e) gSolYawAdj=atof(e)*3.14159265f/180.0f; }
        printf("soldato 3D: ok (%d clip, idle=%d run=%d, h %.2f m -> scala %.2f)\n",
               gSolMdl.clip_count,gSolClipIdle,gSolClipRun,(double)bh,(double)gSolMdlScale);
    } else printf("soldato 3D: sagoma placeholder (assets/models/soldier.glb assente)\n");
#endif
    int mineVpe = gMineM.nv>0 ? gMineM.nv : 36;        // glb, o box placeholder
    int mineMaxV = TRAPS_MAX*mineVpe;
    float *mineBuf=malloc((size_t)mineMaxV*9*sizeof(float));
    GLuint mineVao,mineVbo; glGenVertexArrays(1,&mineVao);glBindVertexArray(mineVao);
    glGenBuffers(1,&mineVbo);glBindBuffer(GL_ARRAY_BUFFER,mineVbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)mineMaxV*9*sizeof(float),NULL,GL_DYNAMIC_DRAW);
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
    // streak dei proiettili: shader dedicato (pos+rgba, additivo), pool renderer-side.
    GLuint trProg=vg_shader("assets/shaders/tracer.vs","assets/shaders/tracer.fs");
    GLint uVPtr=glGetUniformLocation(trProg,"uVP");
    float *trBuf=malloc((size_t)TRACER_POOL*2*7*sizeof(float));
    GLuint trVao,trVbo; glGenVertexArrays(1,&trVao);glBindVertexArray(trVao);
    glGenBuffers(1,&trVbo);glBindBuffer(GL_ARRAY_BUFFER,trVbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)TRACER_POOL*2*7*sizeof(float),NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,7*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,4,GL_FLOAT,0,7*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    // strutture (base + barricate runtime): dinamico, ricostruito ogni frame dallo
    // stato vivo. Allocato SEMPRE con cap fisso generoso (non più sul bbox di scena)
    // così le barricate piazzate a runtime (PLACEMENT_DESIGN) hanno spazio.
    int stMaxV = 8192*30;                                 // ~8k celle struttura
    float *stBuf = malloc((size_t)stMaxV*9*sizeof(float));
    GLuint stVao=0,stVbo=0;
    {   glGenVertexArrays(1,&stVao);glBindVertexArray(stVao);
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
            glVertexAttribPointer(2+i,4,GL_FLOAT,0,14*sizeof(float),(void*)(i*4*sizeof(float)));glVertexAttribDivisor(2+i,1);}
        glEnableVertexAttribArray(5);      // tumble in volo (pitch, roll)
        glVertexAttribPointer(5,2,GL_FLOAT,0,14*sizeof(float),(void*)(12*sizeof(float)));glVertexAttribDivisor(5,1);
        glBindVertexArray(0); A[v].vao=vao;
        free(verts);free(uvs);free(idx);
    }
    glEnable(GL_DEPTH_TEST);
    GLint uVP=glGetUniformLocation(prog,"uVP"),uTS=glGetUniformLocation(prog,"texSize"),uRPF=glGetUniformLocation(prog,"rowsPerFrame"),uHas=glGetUniformLocation(prog,"uHasTex");

    // --- BAKE anello impostor sagome-cadavere (init-time, CORPSE_DESIGN §10.8):
    // RINGVIEWS viste azimutali del modello (heading 0) a elevazione RING_EL,
    // camera a azimuth k·2π/N (stessa convenzione della camera runtime).
    // ALBEDO = griglia (colonna=var*posa × [blocco outfit × vista]); NORMALE
    // world a heading 0 = (colonna × vista), outfit-indipendente, per il relight
    // per heading in corpse_imp.fs. Colonna = v*NPOSE+p; posa 0='dying',
    // 1='death' (devono combaciare con vat_layer_die).
    GLuint bakeProg=vg_shader("assets/shaders/vat.vs","assets/shaders/corpsebake.fs");
    GLint uVPb=glGetUniformLocation(bakeProg,"uVP"),uTSb=glGetUniformLocation(bakeProg,"texSize"),
          uRPFb=glGetUniformLocation(bakeProg,"rowsPerFrame"),uHasb=glGetUniformLocation(bakeProg,"uHasTex"),
          uModeb=glGetUniformLocation(bakeProg,"uMode");
    {   int Wc=RING_CELL*corpseNCols;
        int Ha=RING_CELL*RINGVIEWS*nOutRing, Hn=RING_CELL*RINGVIEWS;   // albedo / normale
        GLuint *tex[2]={&corpseAlb,&corpseNrm}; int texH[2]={Ha,Hn};
        for(int t=0;t<2;t++){
            glGenTextures(1,tex[t]);glBindTexture(GL_TEXTURE_2D,*tex[t]);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,Wc,texH[t],0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        }
        glUseProgram(bakeProg);              // la VP cambia per vista (loop k sotto)
        glUniform1i(glGetUniformLocation(bakeProg,"texPos"),0);
        glUniform1i(glGetUniformLocation(bakeProg,"texNorm"),1);
        glUniform1i(glGetUniformLocation(bakeProg,"texDiff"),2);
        glDisable(GL_BLEND);
        const char *POSE_PFX[VAT_CORPSE_NPOSE]={"dying","death"};
        // due passate: 0 = ALBEDO (nOutRing blocchi × RINGVIEWS righe ciascuno),
        // 1 = NORMALE (RINGVIEWS righe, outfit-indipendente).
        for(int pass=0;pass<2;pass++){
            GLuint dst=pass?corpseNrm:corpseAlb; int Hh=pass?Hn:Ha;
            int nout=pass?1:nOutRing;
            GLuint fbo,depth; glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,dst,0);
            glGenRenderbuffers(1,&depth);glBindRenderbuffer(GL_RENDERBUFFER,depth);
            glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,Wc,Hh);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth);
            if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) fprintf(stderr,"corpse atlas FBO incompleto\n");
            glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
            glUniform1i(uModeb,pass);
            for(int k=0;k<RINGVIEWS;k++){
            float phi=(float)k*6.28318530718f/(float)RINGVIEWS;
            mat4 cproj,cview,cvp;
            m_ortho(cproj,-CORPSE_HALF,CORPSE_HALF,-CORPSE_HALF,CORPSE_HALF,-10,10);
            float rctr[3]={0,RING_LOOKY,0},rup[3]={0,1,0};
            float reye[3]={rctr[0]+6.0f*cosf(RING_EL)*sinf(phi),
                           rctr[1]+6.0f*sinf(RING_EL),
                           rctr[2]+6.0f*cosf(RING_EL)*cosf(phi)};
            m_lookat(cview,reye,rctr,rup); m_mul(cvp,cproj,cview);
            glUniformMatrix4fv(uVPb,1,GL_FALSE,cvp);
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
                        // riga o = outfit base; bake con la versione INSANGUINATA
                        // (o+16), TRANNE gli elementali 14/15 (carbonizzato/
                        // sciolto): terminali, righe 30/31 non autorate.
                        int bo=(o==14||o==15)?o:o+16;
                        float one[14]={0,0,0, 0, 1.0f, (float)fr,(float)fr,0, (float)bo, 0.55f,0.5f,0.5f, 0,0};
                        glViewport(col*RING_CELL,(o*RINGVIEWS+k)*RING_CELL,RING_CELL,RING_CELL);
                        glBindBuffer(GL_ARRAY_BUFFER,bi);glBufferSubData(GL_ARRAY_BUFFER,0,14*sizeof(float),one);
                        glDrawElementsInstanced(GL_TRIANGLES,A[v].ni,GL_UNSIGNED_SHORT,0,1);
                    }
                }
            }
            }   // fine viste k
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

    // --- gore mesh-gibs (FX_LAB): VAO delle mesh di assets/models/gibs.glb + shader mesh
    // statica texturizzata (diffuse di zombie_man, A[0]). Pool fisico in vat_layer.
    GibMesh GM[8]={0}; int nGibMesh=load_gib_meshes("assets/models/gibs.glb",GM,8);
    GLuint mProg=vg_shader("assets/shaders/mesh.vs","assets/shaders/mesh.fs");
#ifdef GAME_SHELL
    heli_load("assets/models/chinook.glb");   // VAO GL: serve il context attivo
#endif
    GLint uVPm=glGetUniformLocation(mProg,"uVP"), uModelm=glGetUniformLocation(mProg,"uModel");
    static float meshgib[256*9];
    printf("mesh-gib: %d mesh da assets/models/gibs.glb\n",nGibMesh);

    // --- particle system (sangue): billboard istanziati, due passate (alpha/add).
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
    anim_init(&gAnim);
#ifdef GAME_SHELL
    // wiring dello ShellHost: i puntatori allo stato di main che il cambio
    // livello (shell_load_level) deve toccare. La UI 2D: VAO/VBO dinamico su
    // flat.vs + ui.fs (alpha in aNormal.x), orto in pixel, depth off.
    gHost.sc=&sc; gHost.s=&s; gHost.g=&g; gHost.dir=&dir;
    gHost.vl=vl; gHost.spctx=&spctx; gHost.dz=&dz;
    gHost.obVbo=obVbo; gHost.prVbo=prVbo; gHost.obNV=&obNV; gHost.prNV=&prNV;
    gHost.ground_on=groundOn; gHost.cam_x=&cx; gHost.cam_z=&cz; gHost.cam_hh=&hh; gHost.running=&running;
    gHost.win=win; gHost.fullscreen=&fullscreen;
    gHost.plc=&plc; gHost.traps=&traps; prep_tabs_build(&plc);   // barra PREP: indice tab->voci (§7)
    gUiBuf=malloc((size_t)UI_MAX_V*9*sizeof(float));
    ui_font_load("assets/fonts/ui.zfnt");   // font vettoriale (fallback font8)
    GLuint uiProg=vg_shader("assets/shaders/flat.vs","assets/shaders/ui.fs");
    GLint uVPui=glGetUniformLocation(uiProg,"uVP");
    glUseProgram(uiProg);
    glUniform1i(glGetUniformLocation(uiProg,"uTex"),0);
    // immagini UI: title screen + pulsanti (hover opzionale), shader dedicato
    gUiTex.title  =ui_tex_load("assets/ui/titlescreen.png",&gUiTex.tw,&gUiTex.th,1);
    gUiTex.btn    =ui_tex_load("assets/ui/pulsante.png",&gUiTex.bw,&gUiTex.bh,1);
    gUiTex.btn_hov=ui_tex_load("assets/ui/pulsante_hover.png",NULL,NULL,0);
    GLuint imgProg=vg_shader("assets/shaders/flat.vs","assets/shaders/uiimg.fs");
    GLint uVPimg=glGetUniformLocation(imgProg,"uVP");
    glUseProgram(imgProg);
    glUniform1i(glGetUniformLocation(imgProg,"uTex"),0);
    GLuint imgVao,imgVbo; glGenVertexArrays(1,&imgVao);glBindVertexArray(imgVao);
    glGenBuffers(1,&imgVbo);glBindBuffer(GL_ARRAY_BUFFER,imgVbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(6*9*sizeof(float)),NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,9*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,0,9*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,9*sizeof(float),(void*)(6*sizeof(float)));glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    GLuint uiVao,uiVbo; glGenVertexArrays(1,&uiVao);glBindVertexArray(uiVao);
    glGenBuffers(1,&uiVbo);glBindBuffer(GL_ARRAY_BUFFER,uiVbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)UI_MAX_V*9*sizeof(float),NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,9*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,0,9*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,9*sizeof(float),(void*)(6*sizeof(float)));glEnableVertexAttribArray(2);
    glBindVertexArray(0);
#endif
    while(running){
#ifdef GAME_SHELL
        // LOOP_DESIGN C: in ASSALTO il piazzamento paga in biomassa (prezzo
        // maggiorato), altrove il budget legacy. Prima degli eventi: click e
        // validate del frame devono già vedere la valuta giusta.
        pl_set_wallet(&plc, gApp.state==APP_ASSAULT ? &gBioWallet : NULL);
#endif
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
#ifdef GAME_SHELL
                // negli stati UI (briefing/settings/…) il mouse non tocca il
                // mondo; TITLE e MENU passano (hover/click sui pulsanti, sotto)
                if(!ed.active && gApp.state!=APP_PREP && gApp.state!=APP_ASSAULT
                   && gApp.state!=APP_TITLE && gApp.state!=APP_MENU)
                    continue;
#endif
                // punto logico -> pixel (corretto anche su HiDPI: SW/SH sono pixel)
                int wpw=1,wph=1; SDL_GetWindowSize(win,&wpw,&wph);
                int motion=(e.type==SDL_EVENT_MOUSE_MOTION);
                float mxf=(motion?e.motion.x:e.button.x)*(float)SW/(wpw>0?wpw:1);
                float myf=(motion?e.motion.y:e.button.y)*(float)SH/(wph>0?wph:1);
                if(motion){ mouse_px=mxf; mouse_py=myf; }
                int alt = (SDL_GetModState()&SDL_KMOD_ALT)!=0;
#ifdef GAME_SHELL
                // TITLE: un click vale "premi un tasto"
                if(!ed.active && gApp.state==APP_TITLE){
                    if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN){
                        au_play(SND_MENU_SELECT);
                        shell_do_act(app_input(&gApp,APP_IN_CONFIRM)); }
                    continue;
                }
                // MENU: hover = selezione (stessi rect del render), click = conferma
                if(!ed.active && gApp.state==APP_MENU){
                    for(int i=0;i<APP_MENU_COUNT;i++){
                        UiRect r; menu_btn_rect(i,(float)SW,(float)SH,&r);
                        if(!ui_hit(&r,mxf,myf)) continue;
                        if(gApp.menu_idx!=i && app_menu_enabled(&gApp,i)){
                            gApp.menu_idx=i; au_play(SND_MENU_MOVE); }
                        if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN &&
                           e.button.button==SDL_BUTTON_LEFT &&
                           app_menu_enabled(&gApp,i) && gApp.menu_idx==i){
                            AppAction act=app_input(&gApp,APP_IN_CONFIRM);
                            if(act!=APP_ACT_NONE && act!=APP_ACT_QUIT)
                                au_play(SND_MENU_SELECT);
                            shell_do_act(act);
                        }
                        break;
                    }
                    continue;
                }
                // barra PREP: il mouse dentro la barra è UI (tab/card/undo/via),
                // MAI un gesto sul mondo (§4 — il bug classico del commit sotto
                // il click di UI). Un drag di linea che finisce in barra muore.
                if(!ed.active && gApp.state==APP_PREP && !drag_cam){
                    prep_ui_layout(SW,SH);
                    if(myf>=gPrepBarY){
                        if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN &&
                           e.button.button==SDL_BUTTON_LEFT)
                            prep_ui_click(mxf,myf,SW,SH,g,s);
                        if(e.type==SDL_EVENT_MOUSE_BUTTON_UP) plineOn=0;
                        continue;
                    }
                }
                // ASSALTO: barra strike (mortaio) + aiming. Dentro barra = UI;
                // in aiming la X segue il mouse e LMB (fuori barra) programma il
                // colpo; RMB o M annulla. Fuori aiming il mouse guida la camera.
                if(!ed.active && gApp.state==APP_ASSAULT && !drag_cam){
                    strikes_ui_layout(SW,SH);
                    if(myf>=gStrikeBarY){
                        if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN &&
                           e.button.button==SDL_BUTTON_LEFT)
                            strikes_ui_click(mxf,myf,SW,SH);
                        if(e.type==SDL_EVENT_MOUSE_BUTTON_UP) plineOn=0;
                        continue;
                    }
                    if(gAimMort){
                        float wx,wy;
                        if(pick_y0(vp,mxf,myf,SW,SH,&wx,&wy)){
                            gAimX=wx; gAimY=wy;
                            if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN){
                                if(e.button.button==SDL_BUTTON_LEFT){
                                    // gittata min/max (BASE_DESIGN §3): fuori si rifiuta
                                    float rdx=wx-gBaseOX, rdy=wy-gBaseOY;
                                    float rd=sqrtf(rdx*rdx+rdy*rdy);
                                    if(rd<gMortMinR || rd>gMortMaxR){
                                        au_play(SND_MENU_MOVE);        // "no": fuori gittata
                                    } else if(gMortCd>0.0f){
                                        au_play(SND_MENU_MOVE);        // "no": tubo ancora caldo
                                    } else if(!bio_take(&gBio,BIO_MORTAR_COST)){
                                        au_play(SND_MENU_MOVE);        // "no": biomassa insufficiente
                                    } else {
                                        gMortCd=gMortCdMax;            // riparte il cooldown
                                        // il colpo parte dalla BOCCA del tubo (modello),
                                        // fallback centro container + BASE_H
                                        float mx_,my_,mz_; mortar_muzzle(&mx_,&my_,&mz_);
                                        strike_add(wx,wy,mx_,mz_,MORTAR_DELAY);
                                        float lo[3]={mx_,my_,mz_};
                                        fx_emit(&fx,lo,&MUZZLE_FLASH_HVY_DEF,0.0f,-1.0f);
                                        fx_emit(&fx,lo,&EXPL_SMOKE_DEF,0.0f,-1.0f);
                                        au_play(SND_MENU_SELECT);
                                    } }
                                else if(e.button.button==SDL_BUTTON_RIGHT) gAimMort=0;
                            }
                        }
                        continue;                  // in aiming il mouse non guida la camera
                    }
                    if(gAimRepair){
                        // RIPARA (§4): LMB TENUTO PREMUTO = flusso biomassa->HP
                        // (repair_channel, nel frame body); qui si tiene solo lo
                        // stato del pulsante. Il bersaglio è quello dell'hover
                        // (piani di quota: prende la torretta su tutta la sagoma);
                        // fallback al pick a terra per un click accanto a un muro
                        // basso. RMB esce dalla modalità.
                        if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN){
                            if(e.button.button==SDL_BUTTON_LEFT){
                                if(gHovSid<0){ float wx,wy;
                                    if(pick_y0(vp,mxf,myf,SW,SH,&wx,&wy))
                                        gHovSid=repair_pick_struct(g,s,wx,wy); }
                                if(gHovSid>=0){ gRepairHold=1; au_play(SND_MENU_SELECT); }
                                else au_play(SND_MENU_MOVE);     // niente struttura sotto
                            } else if(e.button.button==SDL_BUTTON_RIGHT){
                                gRepairHold=0; gAimRepair=0; }
                        } else if(e.type==SDL_EVENT_MOUSE_BUTTON_UP &&
                                  e.button.button==SDL_BUTTON_LEFT) gRepairHold=0;
                        continue;             // in targeting il mouse non guida la camera
                    }
                    if(gAdjOn){               // REGOLA (§4): drag = nuova direzione
                        adjust_event(&e,g,vp,mxf,myf,SW,SH);
                        continue;
                    }
                    // col ghost di costruzione attivo il click è del
                    // piazzamento (soldato-costruttore): il blocco plc sotto.
                    // Con SHIFT premuto il mouse passa alla CAMERA (ruota/pan
                    // anche in modalità: la mira resta congelata, vedi tick).
                    if(gSolMode && !plc.active &&
                       !(SDL_GetModState()&SDL_KMOD_SHIFT)){
                        // SOLDATO: mouse = mira e grilletto
                        if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN){
                            if(e.button.button==SDL_BUTTON_LEFT) gSolFire=1;
                            else if(e.button.button==SDL_BUTTON_RIGHT){
                                // granata: una in aria (il volo è il rate limiter),
                                // gittata clampata dal corpo, costo bio a colpo
                                float wx,wy;
                                if(!gGren.on && gSolTossT<=0.0f &&
                                   gSol && soldier_active(gSol) &&
                                   pick_y0(vp,mxf,myf,SW,SH,&wx,&wy)){
                                    float sx=soldier_x(gSol), sy=soldier_y(gSol);
                                    float dx=wx-sx, dy=wy-sy;
                                    float d=sqrtf(dx*dx+dy*dy);
                                    float rng=gBal.soldier.grenade_range;
                                    if(d>rng && d>1e-3f){
                                        wx=sx+dx*(rng/d); wy=sy+dy*(rng/d); d=rng; }
                                    if(bio_take(&gBio,gBal.soldier.grenade_cost)){
                                        // paga ORA, lancia AL RILASCIO del gesto
                                        gGrenPend.on=1; gGrenPend.x=wx; gGrenPend.y=wy;
                                        gSolTossT=SOL_TOSS_WINDOW;
                                        au_play(SND_MENU_SELECT);
                                    } else au_play(SND_MENU_MOVE); // bio insufficiente
                                } else au_play(SND_MENU_MOVE);     // una alla volta
                            }
                        } else if(e.type==SDL_EVENT_MOUSE_BUTTON_UP &&
                                  e.button.button==SDL_BUTTON_LEFT) gSolFire=0;
                        continue;         // in modalità il mouse non guida la camera
                    }
                    // fuori da ogni modalità: click su una torretta in ricarica =
                    // ricarica ISTANTANEA pagando il costo del suo kind (§5). Il
                    // bersaglio è autoevidente (ha la barra di reload addosso).
                    // Col ghost di costruzione attivo il click è del piazzamento.
                    if(!plc.active && e.type==SDL_EVENT_MOUSE_BUTTON_DOWN &&
                       e.button.button==SDL_BUTTON_LEFT){
                        // hover prima del pick a terra: il click sul corpo del
                        // modello unprojetta DIETRO la torretta
                        int tid=-1;
                        if(gHovTurret>=0){ DefTurret *t=def_turret(g,gHovTurret);
                            if(t && t->mag_size>0 &&
                               (t->mag<t->mag_size ||
                                def_turret_reloading(g,gHovTurret)>0.0f))
                                tid=gHovTurret; }
                        float wx,wy;
                        if(tid<0 && pick_y0(vp,mxf,myf,SW,SH,&wx,&wy))
                            tid=reload_pick_turret(g,wx,wy);
                        if(tid>=0){
                            DefTurret *t=def_turret(g,tid);
                            float cost=BIO_RELOAD_COST((t->kind>=0&&t->kind<4)?t->kind:0);
                            if(bio_take(&gBio,cost)){
                                def_turret_reload_now(g,tid);
                                au_play(SND_MENU_SELECT);
                            } else au_play(SND_MENU_MOVE);       // biomassa insufficiente
                            continue;   // il click era per la torretta, non pan camera
                        }
                    }
                }
                // REGOLA in PREP (§12.Q3): stesso gesto, sul mondo (la barra PREP
                // l'ha già filtrato sopra). Gratis: qui è pura comodità di
                // authoring — il facing si sceglie al piazzamento, ma sbagliarlo
                // non deve costare un undo.
                if(!ed.active && gApp.state==APP_PREP && !drag_cam && gAdjOn){
                    adjust_event(&e,g,vp,mxf,myf,SW,SH);
                    continue;
                }
#endif
                // camera col mouse? PLAY: LMB/RMB nudi; EDIT o placement: solo con Alt.
                int cam_gesture = (!ed.active && !plc.active) || alt;
                if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN && cam_gesture){
                    if(e.button.button==SDL_BUTTON_LEFT){      // pan: ancora il punto sotto il cursore
                        drag_cam=1; float ax,ay;
                        if(pick_y0(vp,mxf,myf,SW,SH,&ax,&ay)){ drag_anx=ax; drag_any=ay; }
                    } else if(e.button.button==SDL_BUTTON_RIGHT){ drag_cam=2; rot_px=mxf; rot_py=myf; }
                    continue;
                }
                if(e.type==SDL_EVENT_MOUSE_BUTTON_UP && drag_cam){ drag_cam=0; continue; }
                if(motion && drag_cam) continue;               // pan/rotate applicati nel frame body
                // --- placement runtime (PLAY, mouse nudo): cursore + LMB commit ---
                if(plc.active && !ed.active){
                    // fase A: fuori PREP il piazzamento si chiude da solo
                    if(gMissionOn && !mission_placement_open(&gMission)){
                        plc.active=0; gTAimOn=0;
#ifdef GAME_SHELL
                        plineOn=0; plineChain=0;   // esistono solo nella shell
#endif
                        continue; }
                    float wx,wy;
#ifdef GAME_SHELL
                    if(sol_builder()){          // ghost inchiodato davanti al soldato
                        sol_build_spot(&wx,&wy); pl_set_cursor(&plc,wx,wy);
                    } else
#endif
                    if(pick_y0(vp,mxf,myf,SW,SH,&wx,&wy)) pl_set_cursor(&plc,wx,wy);
#ifdef GAME_SHELL
                    // soldato-costruttore: LMB piazza al punto davanti a lui
                    // (torretta: facing = la sua mira), RMB chiude il ghost.
                    // Niente gesto d'ancora né linea: un modulo per click.
                    if(sol_builder()){
                        if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN){
                            if(e.button.button==SDL_BUTTON_LEFT){
                                const PlItem *bsel=pl_selected(&plc);
                                if(bsel && bsel->kind==PL_TURRET){
                                    float sx=soldier_x(gSol), sy=soldier_y(gSol);
                                    plc.facing=atan2f(plc.cy-sy,plc.cx-sx);
                                }
                                if(pl_commit(&plc,g,s)){ gStructOn=1;
                                    au_play(SND_MENU_SELECT); }
                                else au_play(SND_MENU_MOVE);   // invalido/bio corta
                            } else if(e.button.button==SDL_BUTTON_RIGHT)
                                plc.active=0;
                        }
                        continue;
                    }
                    // barriere a LINEA (§5): LMB = ancora, rilascio = commit
                    // tutto-o-niente; RMB = annulla il drag (o esce dal ghost).
                    const PlItem *lsel=pl_selected(&plc);
                    if(lsel && lsel->kind==PL_BARRICADE){
                        int shift=(SDL_GetModState()&SDL_KMOD_SHIFT)!=0;
                        if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN){
                            if(e.button.button==SDL_BUTTON_LEFT){
                                if(plineChain){
                                    // polilinea: questo click deposita il vertice
                                    // successivo -> chiude il segmento in sospeso
                                    // dall'ancora corrente. L'ancora del prossimo
                                    // segmento e' QUESTO estremo (spigolo condiviso;
                                    // l'auto-accorciamento in validate tiene il
                                    // giunto senza sovrapposizioni).
                                    PlLinePlan lp;
                                    int okv=pl_line_validate(&plc,g,s,plineAx,plineAy,
                                                             plc.cx,plc.cy,&lp);
                                    if(okv && pl_line_commit(&plc,g,s,plineAx,plineAy,
                                                             plc.cx,plc.cy)){
                                        gStructOn=1; plmod_record(g,&lp,lsel->h);
                                        plineAx=plc.cx; plineAy=plc.cy; }
                                    if(!shift){ plineChain=0; plineOn=0; }  // SHIFT su = fine
                                } else {
                                    plineOn=1; plineAx=plc.cx; plineAy=plc.cy; }  // inizio drag
                            } else if(e.button.button==SDL_BUTTON_RIGHT){
                                if(plineOn||plineChain){ plineOn=0; plineChain=0; }
                                else plc.active=0; }
                        } else if(e.type==SDL_EVENT_MOUSE_BUTTON_UP &&
                                  e.button.button==SDL_BUTTON_LEFT && plineOn && !plineChain){
                            // fine del drag iniziale
                            PlLinePlan lp;   // piano per il registro render (§5)
                            int okv=pl_line_validate(&plc,g,s,plineAx,plineAy,
                                                     plc.cx,plc.cy,&lp);
                            int done=pl_line_commit(&plc,g,s,plineAx,plineAy,plc.cx,plc.cy);
                            if(done){ gStructOn=1; if(okv) plmod_record(g,&lp,lsel->h); }
                            if(shift && done){    // SHIFT giu' = entra in polilinea
                                plineChain=1; plineOn=1; plineAx=plc.cx; plineAy=plc.cy;
                            } else plineOn=0;
                        }
                        continue;
                    }
#endif
                    // torrette: piazzamento DIREZIONALE — LMB (su punto valido)
                    // ancora la posizione, il drag orienta il cono di mira,
                    // il rilascio piazza con quel facing (Placement.facing).
                    // RMB = annulla il gesto in corso, o esce dal piazzamento.
                    { const PlItem *tsel=pl_selected(&plc);
                      if(tsel && tsel->kind==PL_TURRET){
                        if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN){
                            if(e.button.button==SDL_BUTTON_LEFT){
                                if(pl_validate(&plc,g,s)){
                                    gTAimOn=1; gTAimX=plc.cx; gTAimY=plc.cy; }
                            } else if(e.button.button==SDL_BUTTON_RIGHT){
                                if(gTAimOn) gTAimOn=0; else plc.active=0; }
                        } else if(e.type==SDL_EVENT_MOUSE_BUTTON_UP &&
                                  e.button.button==SDL_BUTTON_LEFT && gTAimOn){
                            float dx=plc.cx-gTAimX, dy=plc.cy-gTAimY;
                            if(dx*dx+dy*dy>=0.09f) gTAimFacing=atan2f(dy,dx);
                            pl_set_cursor(&plc,gTAimX,gTAimY);  // commit all'ANCORA
                            plc.facing=gTAimFacing;
                            if(pl_commit(&plc,g,s)) gStructOn=1;
                            gTAimOn=0;
                        }
                        continue;
                      } }
                    if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN){
                        if(e.button.button==SDL_BUTTON_LEFT){ if(pl_commit(&plc,g,s)) gStructOn=1; }
                        else if(e.button.button==SDL_BUTTON_RIGHT){ plc.active=0; }
                    }
                    continue;
                }
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
                            if(ed_place_prop(&ed,&sc,wx,wy)){ destruct_init(&dz,&sc,&gCatalog); prNV=upload_prop_mesh(prVbo,&sc,&gCatalog,&dz,g); }
                        }
                    } else if(e.button.button==SDL_BUTTON_RIGHT){
                        if(ed.npoly>0) ed.npoly--;                 // annulla ultimo vertice
                        else if(ed_delete_at(&sc,wx,wy)){ ed.dirty=1;
                            obNV=upload_obstacle_mesh(obVbo,&sc,!groundOn);
                            destruct_init(&dz,&sc,&gCatalog);
                            prNV=upload_prop_mesh(prVbo,&sc,&gCatalog,&dz,g); }
                    }
                } else if(motion && ed.dragging && hit){ ed.bx=wx; ed.by=wy; }
                else if(e.type==SDL_EVENT_MOUSE_BUTTON_UP &&
                        e.button.button==SDL_BUTTON_LEFT && ed.dragging){
                    ed_commit_drag(&ed,&sc);
                }
                continue;
            }
            if(e.type!=SDL_EVENT_KEY_DOWN) continue;
#ifdef GAME_SHELL
            // --- shell: negli stati UI la tastiera è del menu; in PREP/ASSALTO
            // solo INVIO (via all'assalto) ed ESC (abbandono) sono della shell,
            // il resto va ai controlli di gioco. In EDIT la shell non tocca nulla.
            if(!ed.active){
                AppState ast=gApp.state;
                int in_ui=(ast!=APP_PREP && ast!=APP_ASSAULT);
                int mapped=1; AppInput ain=APP_IN_CONFIRM;
                switch(e.key.key){
                    case SDLK_UP:    case SDLK_W: ain=APP_IN_UP;    break;
                    case SDLK_DOWN:  case SDLK_S: ain=APP_IN_DOWN;  break;
                    case SDLK_LEFT:  case SDLK_A: ain=APP_IN_LEFT;  break;
                    case SDLK_RIGHT: case SDLK_D: ain=APP_IN_RIGHT; break;
                    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
                                                  ain=APP_IN_CONFIRM; break;
                    case SDLK_ESCAPE:             ain=APP_IN_BACK;  break;
                    default: mapped=0; break;
                }
                if(in_ui){
                    if(e.key.key==SDLK_F11){ fullscreen=!fullscreen;
                        SDL_SetWindowFullscreen(win,fullscreen);
                        gApp.fullscreen=fullscreen; continue; }   // settings coerenti
                    if(ast==APP_TITLE){ ain=APP_IN_CONFIRM; mapped=1; }   // any key
                    if(!mapped) continue;
                    if(ain==APP_IN_UP||ain==APP_IN_DOWN) au_play(SND_MENU_MOVE);
                    AppAction act=app_input(&gApp,ain);
                    if(ain==APP_IN_CONFIRM && act!=APP_ACT_NONE && act!=APP_ACT_QUIT)
                        au_play(SND_MENU_SELECT);
                    shell_do_act(act);
                    continue;
                }
                if(e.key.key==SDLK_RETURN||e.key.key==SDLK_KP_ENTER||e.key.key==SDLK_ESCAPE){
                    // LOOP A: in ASSALTO INVIO = "chiama la prossima" (per
                    // app.c CONFIRM in assalto e' comunque un no-op)
                    if(e.key.key!=SDLK_ESCAPE && gApp.state==APP_ASSAULT){
                        host_call_next_wave(); continue; }
                    shell_do_act(app_input(&gApp,
                        e.key.key==SDLK_ESCAPE?APP_IN_BACK:APP_IN_CONFIRM));
                    continue;
                }
            }
#endif
            // --- tasti globali (EDIT e PLAY) ---
            switch(e.key.key){
                case SDLK_ESCAPE: running=0; break;
                case SDLK_F11: fullscreen=!fullscreen; SDL_SetWindowFullscreen(win,fullscreen);
#ifdef GAME_SHELL
                    gApp.fullscreen=fullscreen;                   // settings coerenti
#endif
                    break;
                case SDLK_TAB:                                    // EDIT<->PLAY
                    if(ed.active){
                        if(ed.dirty){ free_world(s,g,dir);        // re-instanzia dalla Scene editata
                            if(build_world(&sc,vl,fillN,&spctx,&s,&g,&dir)!=0){running=0;break;}
                            destruct_init(&dz,&sc,&gCatalog);     // ri-stato distruttibili
                            obNV=upload_obstacle_mesh(obVbo,&sc,!groundOn);
                            prNV=upload_prop_mesh(prVbo,&sc,&gCatalog,&dz,g); ed.dirty=0; }
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
                case SDLK_E:  // blast di debug al PUNTATORE (non più al centro-camera)
                    if(pick_y0(vp,mouse_px,mouse_py,SW,SH,&blast_x,&blast_y)) blast_pending=1;
                    break;
#ifdef GAME_SHELL
                // i tre verbi dell'assalto (BIOMASS §4): modalità esclusive
                case SDLK_M:  // MORTAIO: entra/esci dall'aiming
                    if(gApp.state==APP_ASSAULT){
                        bio_mode_set(!gAimMort,0,0);
                        if(gAimMort) plc.active=0;    // esclusiva col ghost
                        au_play(gAimMort?SND_MENU_SELECT:SND_MENU_MOVE); }
                    break;
                case SDLK_R:  // RIPARA: tieni premuto su una struttura = flusso bio->HP
                    if(gApp.state==APP_ASSAULT){
                        bio_mode_set(0,!gAimRepair,0);
                        if(gAimRepair) plc.active=0;  // esclusiva col ghost
                        au_play(gAimRepair?SND_MENU_SELECT:SND_MENU_MOVE); }
                    break;
                case SDLK_V:  // REGOLA: riorienta una torretta (anche in PREP, §12.Q3)
                    if(gApp.state==APP_ASSAULT || gApp.state==APP_PREP){
                        int on=!gAdjOn;
                        if(sol_builder()){ sol_verbs_off(); gAdjOn=on; }
                        else bio_mode_set(0,0,on);
                        if(gAdjOn) plc.active=0;      // esclusiva col ghost di piazzamento
                        au_play(gAdjOn?SND_MENU_SELECT:SND_MENU_MOVE); }
                    break;
                case SDLK_F:  // SOLDATO: entra/esci (servito nel frame body)
                    if(gApp.state==APP_ASSAULT) gSolWant=1;
                    break;
                case SDLK_L:  // card COSTRUISCI: TORRETTA L (LOOP_DESIGN C)
                    if(gApp.state==APP_ASSAULT &&
                       (!gMissionOn || mission_placement_open(&gMission)))
                        build_card_toggle(0);
                    break;
#endif
                case SDLK_RETURN: case SDLK_KP_ENTER:  // fase A: via all'assalto
                    if(gMissionOn && gMission.state==MISSION_PREP){
                        mission_go(&gMission); plc.active=0;
                        printf("missione: ASSALTO\n"); }
                    else if(gMissionOn && gMission.state==MISSION_ASSAULT)
                        host_call_next_wave();         // LOOP A: chiama la prossima
                    break;
                case SDLK_P:    // piazzamento runtime: attiva/disattiva (budget di prova se 0)
                    if(gMissionOn && !mission_placement_open(&gMission)){
                        printf("placement: solo in PREP (fase A)\n"); break; }
                    plc.active=!plc.active; gTAimOn=0;
                    if(plc.active && def_budget(g)<=0){ def_set_budget(g,99999); printf("placement ON (budget di prova)\n"); }
                    break;
#ifdef GAME_SHELL
                // [ ] ciclano la tab PREP: in assalto la scelta è solo le card
                // (il ciclo uscirebbe dal sottoinsieme di LOOP_DESIGN C)
                case SDLK_LEFTBRACKET:
                    if(plc.active && gApp.state==APP_PREP) prep_cycle(-1); break;
                case SDLK_RIGHTBRACKET:
                    if(plc.active && gApp.state==APP_PREP) prep_cycle(+1); break;
                case SDLK_1: if(gApp.state==APP_PREP) prep_tab_set(0,g); break;
                case SDLK_2: if(gApp.state==APP_PREP) prep_tab_set(1,g); break;
                case SDLK_3: if(gApp.state==APP_PREP) prep_tab_set(2,g); break;
                case SDLK_Z:            // Ctrl+Z = undo del piazzamento (§6)
                    if((e.key.mod&SDL_KMOD_CTRL) && gApp.state==APP_PREP){
                        if(pl_undo_pop(&plc,g,s)) gStructOn=1;
                        plmod_trim(g); }
                    break;
#else
                case SDLK_LEFTBRACKET:  if(plc.active) pl_cycle(&plc,-1); break;
                case SDLK_RIGHTBRACKET: if(plc.active) pl_cycle(&plc,+1); break;
#endif
                case SDLK_COMMA:  if(plc.active) pl_rotate(&plc,-1); break;
                case SDLK_PERIOD: if(plc.active) pl_rotate(&plc,+1); break;
                case SDLK_B:{   // in ASSALTO (shell): card BARRICATA; altrove
                                // il debug: cassonetto draggable sotto il cursore
#ifdef GAME_SHELL
                    if(gApp.state==APP_ASSAULT){
                        if(!gMissionOn || mission_placement_open(&gMission))
                            build_card_toggle(1);
                        break; }
#endif
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
        int sim_run = !ed.active && !paused;
        int combat = 1;      // 0 = sim viva ma il mondo non si fa danni (EXTRACT)
#ifdef GAME_SHELL
        // in EXTRACT la sim CONTINUA (l'orda si muove sotto l'heli che parte)
        // ma combat=0: niente def_update (zero danni a strutture, torrette
        // mute), niente mine (2026-07-12). In PREP idem (2026-07-14): le
        // torrette sono spente finché l'assalto non parte — senza il gate
        // sparavano ai branchi dormienti durante il piazzamento.
        sim_run = sim_run && (gApp.state==APP_PREP || gApp.state==APP_ASSAULT
                              || gApp.state==APP_EXTRACT);
        combat = (gApp.state==APP_ASSAULT);
        if(gBioFlash>0.0f) gBioFlash-=(float)frame_t;   // flash HUD dello spreco
        if(gWaveMsgT>0.0f) gWaveMsgT-=(float)frame_t;   // flash bonus "chiama"
        if(gMortCd>0.0f){ gMortCd-=(float)frame_t; if(gMortCd<0.0f) gMortCd=0.0f; }
        // RIPARA a mantenimento (§4): la spesa è un flusso, va nel frame body
        // (dt reale) — non negli eventi. Fuori dall'assalto non si ripara.
        if(gApp.state==APP_ASSAULT) repair_channel(g,(float)frame_t);
#endif
        host_apply_mags(g);   // equipaggia i caricatori delle torrette nuove (§5)
        if(sim_run){
            if(blast_pending){
                // host_blast = def_blast (danno/impulso/strutture/cadaveri, +eventi
                // gib/sangue) + FX §6 + burst dei prop decor. Il re-upload della
                // mesh prop serve perché destruct_force non lo intercetta da solo.
                if(host_blast(g,s,&sc,&gCatalog,&dz,&fx,vl,blast_x,blast_y,BLAST_R,BLAST_DMG,blast_str,blast_up))
                    prNV=upload_prop_mesh(prVbo,&sc,&gCatalog,&dz,g);
                blast_pending=0; printf("blast @ (%.1f,%.1f) R %.1f D %.0f str %.1f up %.2f\n",
                    (double)blast_x,(double)blast_y,(double)BLAST_R,(double)BLAST_DMG,
                    (double)blast_str,(double)blast_up);
            }
            acc_t+=frame_t;
            while(acc_t>=FIXED_DT){
                // §8: il director emette l'ondata (burst-free). In benchmark
                // (fillN) il campo è già pieno: niente director. Con la shell
                // le ondate partono solo in ASSALTO (in PREP si fortifica).
                int dir_on=1;
#ifdef GAME_SHELL
                dir_on=(gApp.state==APP_ASSAULT);
#endif
                if(dir && dir_on && simp_count(s)<MAXA) def_director_update(dir,FIXED_DT);
                // missione (fase A): la macchina aggiorna i SUOI director
                // (solo in ASSAULT) e decide WIN/LOSE. Con la shell comanda
                // la shell: in APP_PREP la missione resta congelata (niente
                // auto-timer), INVIO -> START_ASSAULT -> mission_go.
                if(gMissionOn){
#ifdef GAME_SHELL
                    if(gApp.state==APP_ASSAULT) mission_update(&gMission,FIXED_DT);
#else
                    mission_update(&gMission,FIXED_DT);
#endif
                }
#ifdef GAME_SHELL
                // --- soldato (SOLDIER_DESIGN): tick PRIMA di simp_step (scrive
                // la velocità di drive che lo step integra). Invarianti: corpo
                // solo in modalità, modalità solo in ASSALTO (bio_mode_set
                // spegne gSolMode ma non può rimuovere il corpo: lo si fa qui).
                if(gSol){
                    if(gSolWant){                    // toggle da F o card SOLDATO
                        gSolWant=0;
                        if(gSolMode){ gSolMode=0; gSolFire=0;
                                      au_play(SND_MENU_MOVE); }
                        else if(gApp.state==APP_ASSAULT &&
                                soldier_down(gSol)<=0.0f){
                            float dxp,dyp;           // primo punto libero attorno alla base
                            if(soldier_spot(s,gBaseOX,gBaseOY,
                                            gBal.soldier.radius,&dxp,&dyp) &&
                               soldier_deploy(gSol,dxp,dyp)){
                                bio_mode_set(0,0,0); // esclusiva coi verbi...
                                gSolMode=1;          // ...che azzera anche gSolMode
                                plc.active=0;        // ...e col ghost di costruzione
                                gSolLastX=dxp; gSolLastY=dyp;
                                gGunMuzOn=0;         // bocca stantia del deploy prima
                                au_play(SND_MENU_SELECT);
                            } else au_play(SND_MENU_MOVE);   // base assediata/lockout
                        } else au_play(SND_MENU_MOVE);
                    }
                    if(gSolMode && gApp.state!=APP_ASSAULT){ gSolMode=0; gSolFire=0; }
                    if(!gSolMode && soldier_active(gSol))    // rientro alla base
                        soldier_recall(gSol);
                    float mvx=0.0f,mvy=0.0f;
                    if(gSolMode){
                        // WASD relativo allo SCHERMO (la camera è ruotata di az):
                        // W = verso l'alto dello schermo proiettato a terra
                        const bool *ks=SDL_GetKeyboardState(NULL);
                        float fwd=(ks[SDL_SCANCODE_W]?1.0f:0.0f)-(ks[SDL_SCANCODE_S]?1.0f:0.0f);
                        float rgt=(ks[SDL_SCANCODE_D]?1.0f:0.0f)-(ks[SDL_SCANCODE_A]?1.0f:0.0f);
                        float sn=sinf(az), cn=cosf(az);
                        mvx=fwd*(-sn)+rgt*( cn);
                        mvy=fwd*(-cn)+rgt*(-sn);
                        if(gAimMort||gAimRepair){
                            // verbo col mouse attivo: il soldato resta FERMO
                            // dove sta (mira congelata, niente fuoco) finché
                            // il verbo non si chiude — fare in fretta, il
                            // lure continua ad attirare l'orda su di lui.
                            mvx=0.0f; mvy=0.0f; gSolFire=0;
                        }
                        // SHIFT = mouse alla camera: mira congelata, niente fuoco
                        else if(ks[SDL_SCANCODE_LSHIFT]||ks[SDL_SCANCODE_RSHIFT])
                            gSolFire=0;
                        else
                            pick_y0(vp,mouse_px,mouse_py,SW,SH,&gSolAimX,&gSolAimY);
                        if(gSolTossT>0.0f){   // gesto granata: fermo, faccia
                            mvx=0.0f; mvy=0.0f;                 // al bersaglio
                            if(gGrenPend.on){ gSolAimX=gGrenPend.x;
                                              gSolAimY=gGrenPend.y; }
                        }
                    }
                    // gesto granata: avanza col tick (gioco, non fiction — gata
                    // movimento e spawn); morso/recall a metà = annullo+rimborso
                    if(gSolTossT>0.0f){
                        gSolTossT-=FIXED_DT;
                        if(gSolMode && soldier_active(gSol)){
                            if(gGrenPend.on &&
                               SOL_TOSS_WINDOW-gSolTossT>=SOL_TOSS_RELEASE){
                                float sx=soldier_x(gSol), sy=soldier_y(gSol);
                                float dxp=gGrenPend.x-sx, dyp=gGrenPend.y-sy;
                                float d=sqrtf(dxp*dxp+dyp*dyp);
                                gGren.on=1; gGren.ox=sx; gGren.oy=sy;
                                gGren.x=gGrenPend.x; gGren.y=gGrenPend.y;
                                gGren.ttot=0.45f+0.05f*d; gGren.t=gGren.ttot;
                                gGrenPend.on=0;
                            }
                        } else {                    // interrotto prima del lancio
                            if(gGrenPend.on){ gGrenPend.on=0;
                                bio_add(&gBio,gBal.soldier.grenade_cost); }
                            gSolTossT=0.0f;
                        }
                    }
                    // SCAVALCAMENTO: WASD tenuto contro una cella-struttura
                    // scavalcabile per SOL_CLIMB_PUSH s -> vault. Atterraggio =
                    // primo punto libero oltre il muro lungo la spinta; muri
                    // più spessi di ~3 celle (doppie cinte accostate) non si
                    // scavalcano. Durata = clip climb + jump_down native.
                    if(gSolMode && soldier_active(gSol) &&
                       !soldier_climbing(gSol) && gSolTossT<=0.0f){
                        float ml=sqrtf(mvx*mvx+mvy*mvy); int push=0;
                        if(ml>0.5f){
                            float dxn=mvx/ml, dyn=mvy/ml;
                            float sx=soldier_x(gSol), sy=soldier_y(gSol);
                            float cs=sc.cell, t0=gBal.soldier.radius+0.30f;
                            int pcx=(int)((sx+dxn*t0)/cs), pcy=(int)((sy+dyn*t0)/cs);
                            int id=def_cell_struct(g,pcx,pcy);
                            if(id>=0 && simp_is_wall(s,pcx,pcy) &&
                               !def_struct_is_turret(g,id) && id!=gLzCore &&
                               !(id<PROP_WORLD_MAX_STRUCT && gPropW.struct_is_prop[id])){
                                push=1; gSolPushT+=FIXED_DT;
                                if(gSolPushT>=SOL_CLIMB_PUSH){
                                    float d=t0;
                                    while(d<=t0+1.6f &&
                                          simp_is_wall(s,(int)((sx+dxn*d)/cs),
                                                         (int)((sy+dyn*d)/cs)))
                                        d+=0.20f;
                                    int okl=0; float ex=0,ey=0;
                                    if(d<=t0+1.6f){
                                        float r=gBal.soldier.radius;
                                        for(float e=d+r+0.05f;e<=d+r+2.0f;e+=0.25f){
                                            float qx=sx+dxn*e, qy=sy+dyn*e;
                                            if(simp_free_at(s,qx,qy,r)){
                                                ex=qx; ey=qy; okl=1; break; }
                                        }
                                    }
                                    float dc=gSolClipClimb>=0?
                                             gSolMdl.clips[gSolClipClimb].duration:1.8f;
                                    float dj=gSolClipJump>=0?
                                             gSolMdl.clips[gSolClipJump].duration:1.0f;
                                    if(okl && soldier_climb_begin(gSol,ex,ey,dc+dj)){
                                        gSolClimb.on=1; gSolClimb.t=0.0f;
                                        gSolClimb.x0=sx; gSolClimb.y0=sy;
                                        gSolClimb.yaw=atan2f(dxn,dyn);
                                        gSolClimb.ex=ex; gSolClimb.ey=ey;
                                        gSolYaw=gSolClimb.yaw;   // niente piroetta
                                        gSolFire=0;
                                    }
                                    gSolPushT=0.0f;
                                }
                            }
                        }
                        if(!push) gSolPushT=0.0f;
                    }
                    if(gSolClimb.on){                 // orologio della fiction
                        gSolClimb.t+=FIXED_DT;
                        if(!gSol || !soldier_active(gSol) ||
                           !soldier_climbing(gSol))   // atterrato o recall
                            gSolClimb.on=0;
                    }
                    int wasA=soldier_active(gSol);
                    SolShotCtx sctx={g,s};
                    gSolFiring = gSolMode&&gSolFire&&combat&&!plc.active
                                 &&gSolTossT<=0.0f&&!soldier_climbing(gSol);
                    // heading: il soldato guarda la MIRA solo quando spara
                    // (+ una coda SOL_AIM_LINGER che assorbe il tap-fire);
                    // altrimenti guarda dove cammina (2026-07-19, da testare —
                    // il render legge gSolAimT).
                    if(gSolFiring) gSolAimT=SOL_AIM_LINGER;
                    else if(gSolAimT>0.0f) gSolAimT-=FIXED_DT;
                    soldier_step(gSol,mvx,mvy,gSolAimX,gSolAimY,
                                 gSolFiring,FIXED_DT,on_soldier_shot,&sctx);
                    if(gGunFlash){        // vampa armata da on_soldier_shot
                        gGunFlash=0;
                        if(gGunMuzOn)
                            fx_emit(&fx,gGunMuz,&MUZZLE_FLASH_DEF,
                                    gGunFlashAng,0.30f);
                    }
                    if(soldier_active(gSol)){
                        gSolLastX=soldier_x(gSol); gSolLastY=soldier_y(gSol);
                    } else if(wasA && gSolMode){     // sbranato: fuori modalità
                        gSolMode=0; gSolFire=0;
                        float lo[3]={gSolLastX,
                                     ter_z(gSolLastX,gSolLastY)+0.9f,gSolLastY};
                        fx_emit(&fx,lo,&BLOOD_BURST_DEF,0.0f,-1.0f);
                        au_play(SND_MENU_MOVE);
                    }
                    // granata in volo: arco + fiction billboard come il mortaio;
                    // all'impatto host_blast coi numeri soldier (resa bio scalata)
                    if(gGren.on){
                        gGren.t-=FIXED_DT;
                        if(gGren.t<=0.0f){ gGren.on=0;
                            gBioYieldMul=gBal.soldier.bio_yield;
                            int pch=host_blast(g,s,&sc,&gCatalog,&dz,&fx,vl,
                                gGren.x,gGren.y,gBal.soldier.grenade_r,
                                gBal.soldier.grenade_damage,
                                gBal.soldier.grenade_strength,gBal.soldier.grenade_up);
                            gBioYieldMul=1.0f;
                            if(pch) prNV=upload_prop_mesh(prVbo,&sc,&gCatalog,&dz,g);
                        } else {
                            float p=1.0f-gGren.t/gGren.ttot;   // 0 lancio -> 1 impatto
                            float ax=gGren.ox+(gGren.x-gGren.ox)*p;
                            float azp=gGren.oy+(gGren.y-gGren.oy)*p;
                            float h0=ter_z(gGren.ox,gGren.oy)+1.2f;
                            float h1=ter_z(gGren.x,gGren.y)+0.2f;
                            float ay=h0+(h1-h0)*p+GREN_APEX*4.0f*p*(1.0f-p);
                            float hp3[3]={ax,ay,azp};
                            float c0[4]={0.38f,0.45f,0.32f,1.0f};
                            float c1[4]={0.25f,0.30f,0.22f,0.0f};
                            float zv[3]={0,0,0};
                            fx_emit_one(&fx,hp3,zv,0.12f,0.0f,0.0f,c0,c1,
                                        0.30f,0.16f,-1,0.0f,false,FX_BLEND_ALPHA);
                        }
                    }
                }
#endif
                Uint64 t0=SDL_GetPerformanceCounter();
                simp_step(s,FIXED_DT);
                Uint64 t1=SDL_GetPerformanceCounter();
                if(combat) def_update(g,FIXED_DT);
                // mine (GAME_PLAN fase D): dopo lo step (griglia fresca) le trappole
                // ARMATE scattano sul primo agente vicino -> host_blast (def_blast +
                // FX, friendly fire incluso). Catene risolte in ordine di id nel core.
                if(combat){ TrapBlastCtx tbc={g,s,&sc,&gCatalog,&dz,&fx,vl,0};
                  traps_update(&traps,s,FIXED_DT,on_trap_blast,&tbc);
                  if(tbc.prop_changed) prNV=upload_prop_mesh(prVbo,&sc,&gCatalog,&dz,g); }
#ifdef GAME_SHELL
                // mortaio che MIRA (BASE_DESIGN §3): in aiming lo stand insegue
                // l'azimut del cursore e il tubo si inclina all'alzo balistico
                // dell'arco reale (pendenza al lancio di una parabola con apex
                // MORTAR_APEX sulla gittata: elev = atan(4·apex/d), alzo dal
                // verticale = 90° − elev). Slew limitato = fiction meccanica;
                // il click spara comunque subito (mentre miri il tubo è già lì).
                if(gLzCore>=0 && gBaseM.ok){
                    float taz=gMortAz, ttl=gMortTilt;
                    if(gAimMort && gApp.state==APP_ASSAULT){
                        float adx=gAimX-gBaseOX, ady=gAimY-gBaseOY;
                        float ad=sqrtf(adx*adx+ady*ady);
                        if(ad>1e-3f){
                            taz=atan2f(ady,adx);
                            float dc=ad; if(dc<gMortMinR)dc=gMortMinR; if(dc>gMortMaxR)dc=gMortMaxR;
                            ttl=1.5707963f-atanf(4.0f*MORTAR_APEX/dc);
                        }
                    }
                    float da=taz-gMortAz;
                    while(da> 3.14159265f) da-=6.28318531f;
                    while(da<-3.14159265f) da+=6.28318531f;
                    float ms=2.4f*FIXED_DT;                    // rad/s dello stand
                    if(da>ms)da=ms;
                    if(da<-ms)da=-ms;
                    gMortAz+=da;
                    float dl=ttl-gMortTilt, ls=1.8f*FIXED_DT;  // rad/s dell'alzo
                    if(dl>ls)dl=ls;
                    if(dl<-ls)dl=-ls;
                    gMortTilt+=dl;
                }
                // colpi di mortaio in volo (BASE_DESIGN §3): il proiettile parte dal
                // container (ox,oy) e segue un arco parabolico fino al bersaglio (x,y),
                // che raggiunge esattamente a t<=0 -> host_blast (impatto). Il proiettile
                // è FICTION: una testata billboard luminosa + scia di fumo emessa lungo
                // il percorso (fx_emit_one, niente stato persistente). Precise, no RNG.
                for(int i=0;i<STRIKE_MAX;i++) if(gStrikes[i].on){
                    gStrikes[i].t-=FIXED_DT;
                    if(gStrikes[i].t<=0.0f){ gStrikes[i].on=0;
                        // LOOP_DESIGN D: resa bio ridotta sui kill del colpo +
                        // il cratere timbra sangue-paura (mortaio = nega la
                        // strada per ~danger_hl, non farm di biomassa).
                        gBioYieldMul=gBal.mortar.bio_yield;
                        int pch=host_blast(g,s,&sc,&gCatalog,&dz,&fx,vl,gStrikes[i].x,gStrikes[i].y,
                                           BLAST_R,BLAST_DMG,blast_str,blast_up);
                        gBioYieldMul=1.0f;
                        if(gBal.mortar.fear_w>0.0f && gBal.mortar.fear_r>0.0f)
                            simp_add_danger(s,gStrikes[i].x,gStrikes[i].y,
                                            gBal.mortar.fear_r,gBal.mortar.fear_w);
                        if(pch) prNV=upload_prop_mesh(prVbo,&sc,&gCatalog,&dz,g);
                        continue; }
                    // posizione lungo l'arco a questo step
                    float p=1.0f-gStrikes[i].t/gStrikes[i].ttot;   // 0 lancio -> 1 impatto
                    float ox=gStrikes[i].ox, oy=gStrikes[i].oy, tx=gStrikes[i].x, ty=gStrikes[i].y;
                    float ax=ox+(tx-ox)*p, az=oy+(ty-oy)*p;        // XZ (mondo: x, z=mappa-y)
                    float h0=ter_z(ox,oy)+BASE_H, h1=ter_z(tx,ty)+0.2f;
                    float ay=h0+(h1-h0)*p + MORTAR_APEX*4.0f*p*(1.0f-p);
                    float hp3[3]={ax,ay,az};
                    // testata: puntino brillante additivo, fermo e a vita breve -> scia-cometa
                    float hc0[4]={1.0f,0.95f,0.65f,1.0f}, hc1[4]={1.0f,0.55f,0.15f,0.0f};
                    float zerov[3]={0,0,0};
                    fx_emit_one(&fx,hp3,zerov, 0.18f, 0.0f,0.0f, hc0,hc1, 0.55f,0.22f, -1, 0.0f,false,FX_BLEND_ADD);
                    // fumo di scia: grigio, sale piano, sfuma
                    float sc0[4]={0.55f,0.55f,0.58f,0.7f}, sc1[4]={0.35f,0.35f,0.38f,0.0f};
                    float sv[3]={0,0.6f,0};
                    fx_emit_one(&fx,hp3,sv, 0.7f, 0.0f,1.2f, sc0,sc1, 0.4f,1.1f, -1, 0.0f,false,FX_BLEND_ALPHA);
                }
#endif
                // danno da caduta (EXPLOSION_DESIGN §3.4): gli agenti atterrati in
                // questo step (lanciati da un blast/impulso) prendono danno costante
                // → ferite/gib+sangue via i soliti eventi. Chiude il buco M3.2
                // (simp_landed non era mai consumato dall'host).
                // Atterraggio: se la caduta è LETALE lo zombie si SMEMBRA in gib
                // (cubetti che svaniscono + pozza di sangue, niente clip di morte
                // né cadavere — BASE-side vat_layer_gib); altrimenti incassa il
                // danno e prosegue. (side note utente 2026-07-07)
                { int nl=simp_landed_count(s); const SimPHandle *lh=simp_landed(s);
                  const int *hp=def_hp(g); int fd=(int)(FALL_DMG+0.5f);
                  for(int k=0;k<nl;k++){
                      int i=simp_index_of(s,lh[k]); if(i<0) continue;
                      int slot=simp_slot_of(s,i);
                      if(hp[slot]<=fd) def_gib_agent(g,lh[k]);   // caduta letale -> smembra
                      else {
                          def_damage_agent(g,lh[k],FALL_DMG);
                          // sopravvissuto: puff di polvere ai piedi per mascherare
                          // lo stacco volo->camminata.
                          float lx=simp_px(s)[i], ly=simp_py(s)[i];
                          float lo[3]={lx, ter_z(lx,ly)+0.05f, ly};
                          fx_emit(&fx,lo,&LAND_DUST_DEF,0.0f,-1.0f);
                      }
                  } }
                // rinculo torrette + vampa alla bocca + streak del proiettile:
                // per chi ha sparato in questo step. build_turret_mesh legge il
                // rinculo; il tracer parte dalla bocca del cannone verso l'impatto.
                if(combat){ int nt=def_turret_count(g);   // t->fired stantio senza def_update
                  for(int ti=0;ti<nt;ti++){ DefTurret *t=def_turret(g,ti);
                      if(!t->fired || def_turret_disabled(g,ti)) continue;
                      float ca=cosf(t->ang), sa=sinf(t->ang);
                      int mk=(t->kind>=0&&t->kind<4)?t->kind:0;
                      TurretModel *tm=&gTurM[mk];
                      float mh=tm->ok?tm->muzzle_h:0.9f, mx=tm->ok?tm->muzzle_x:0.0f;
                      float ox=t->x+ca*mx, oz=t->y+sa*mx, oy=mh+ter_z(t->x,t->y);
                      float mo[3]={ox,oy,oz};
                      if(t->kind==TUR_FLAME){
                          // getto continuo: niente tracer, rinculo appena accennato
                          anim_fire(&gAnim,ANIM_TURRET_RECOIL,ti,0.04f);
                          fx_emit(&fx,mo,&FLAME_JET_DEF,t->ang,t->cone_half);
                          fx_emit(&fx,mo,&FLAME_JET_SMOKE_DEF,t->ang,t->cone_half*1.3f);
                      } else if(t->kind==TUR_ACID){
                          anim_fire(&gAnim,ANIM_TURRET_RECOIL,ti,0.05f);
                          fx_emit(&fx,mo,&ACID_JET_DEF,t->ang,t->cone_half);
                      } else {
                          anim_fire(&gAnim,ANIM_TURRET_RECOIL,ti,0.12f);
                          float ex=t->x+ca*t->last_t, ez=t->y+sa*t->last_t, ey=mh+ter_z(ex,ez);
                          tracer_spawn(ox,oy,oz, ex,ey,ez, t->heavy);
                          fx_emit(&fx,mo, t->heavy?&MUZZLE_FLASH_HVY_DEF:&MUZZLE_FLASH_DEF, t->ang, 0.35f);
                      }
                  } }
                anim_update(&gAnim,FIXED_DT);
                tracer_step(&fx,FIXED_DT);          // avanza gli streak, scintilla all'arrivo
#ifdef GAME_SHELL
                if(gApp.state==APP_ASSAULT){                 // esito missione
                    const AppLevel *SL=app_cur_level(&gApp);
                    gSurviveT+=FIXED_DT;
                    int lost, won;
                    if(gMissionOn){                          // fase A: comanda mission.c
                        lost=(gMission.state==MISSION_LOST);
                        won =(gMission.state==MISSION_WON);
                    } else {                                 // legacy: timer AppLevel
                        lost=def_lost(g);
                        won =(SL && gSurviveT>=SL->survive_s);
                    }
                    if(lost || won){
                        au_play(lost?SND_LOSE:SND_WIN);
                        shell_do_act(app_report_result(&gApp,!lost));
                        // vinta -> cinematica di ESTRAZIONE (BASE_DESIGN §4.2);
                        // senza lz/modello/base viva si passa dritti al debrief.
                        if(gApp.state==APP_EXTRACT){
                            // def_update non gira più in EXTRACT: rimuovi i
                            // timbri lure attivi ORA (bit-esatto) o restano
                            // inchiodati nella nav per tutta la cinematica.
                            def_set_fire_lure(g,0.0f,0.0f,0.0f);
                            if(sc.has_lz && gHeliM.ok && gLzCore>=0 &&
                               !def_struct_collapsed(g,gLzCore)){
                                heli_begin(2,&sc);
                                // niente snap: camera-follow progressivo (sotto)
                            } else shell_do_act(app_input(&gApp,APP_IN_CONFIRM));
                        }
                    }
                }
#endif
                // prop distruttibili (DESTRUCT_DESIGN.md): contatto -> scoppio FX.
                // dopo def_update (la griglia puo' essere stantia dai kill: la query
                // cade su brute-force, corretta). Re-upload se cambia o sta cadendo.
                if(destruct_update(&dz,s,&sc,FIXED_DT,on_prop_burst,&dctx)
                   || destruct_animating(&dz))
                    prNV=upload_prop_mesh(prVbo,&sc,&gCatalog,&dz,g);
                // prop assediabili (§6): ridisegna al cambio di HP (scurimento
                // col danno, sparizione al crollo). La somma cambia solo sotto
                // assedio: il re-upload resta un evento raro.
                { static float hpSum=-1.0f; float cur=0.0f;
                  for(int pi=0;pi<gPropW.n;pi++){ int psid=gPropW.struct_id[pi];
                      if(psid>=0) cur+=def_struct_hp(g,psid); }
                  if(cur!=hpSum){ prNV=upload_prop_mesh(prVbo,&sc,&gCatalog,&dz,g); hpSum=cur; } }
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
                // incendiati/corrosi (torrette 2.0, Blocco 2): fuoco+fumo (o
                // fumi verdi acidi) ADDOSSO agli agenti con status elementale,
                // stile heli (fx_emit_one, zero stato persistente). Ogni agente
                // emette 1 volta ogni 4 step (~15 Hz): pool FX sotto controllo
                // anche con centinaia di roghi. Jitter pseudo-random da hash
                // slot+frame (gli FX sono solo visivi, niente RNG di sim).
                { const uint8_t *stt=def_status(g); int nn=simp_count(s);
                  static unsigned stFrame=0; stFrame++;
                  for(int i=0;i<nn;i++){ int slot=simp_slot_of(s,i);
                      if(stt[slot]==DST_NONE) continue;
                      if(((unsigned)slot+stFrame)&3u) continue;
                      float x=simp_px(s)[i], y=simp_py(s)[i];
                      unsigned hsh=(unsigned)slot*2654435761u ^ stFrame*0x9E3779B9u;
                      float jx=((float)(hsh&255u)/255.0f-0.5f)*0.35f;
                      float jy=((float)((hsh>>8)&255u)/255.0f-0.5f)*0.35f;
                      float o[3]={x+jx, ter_z(x,y)+0.5f+((float)((hsh>>16)&255u)/255.0f)*0.9f, y+jy};
                      float vv[3]={jx*2.0f, 1.6f, jy*2.0f};
                      if(stt[slot]==DST_BURNING){
                          float c0[4]={1.0f,0.70f,0.18f,0.9f}, c1[4]={0.85f,0.10f,0.02f,0.0f};
                          fx_emit_one(&fx,o,vv,0.45f,-2.0f,0.8f,c0,c1,0.28f,0.55f,-1,0.2f,false,FX_BLEND_ADD);
                          if(((hsh>>20)&7u)==0){               // sbuffo di fumo saltuario
                              float s0[4]={0.15f,0.13f,0.12f,0.35f}, s1[4]={0.10f,0.10f,0.10f,0.0f};
                              float sv[3]={jx, 1.2f, jy};
                              fx_emit_one(&fx,o,sv,1.4f,-1.0f,1.0f,s0,s1,0.35f,1.0f,-1,0.6f,false,FX_BLEND_ALPHA);
                          }
                      } else {                                 // DST_ACID: fumi verdi
                          float c0[4]={0.35f,0.85f,0.20f,0.45f}, c1[4]={0.15f,0.45f,0.10f,0.0f};
                          fx_emit_one(&fx,o,vv,1.0f,-1.2f,1.0f,c0,c1,0.25f,0.75f,-1,0.5f,false,FX_BLEND_ALPHA);
                      }
                  } }
                // crollo torretta (transizione viva->distrutta): sbuffo di fumo
                // + scintille one-shot, poi un emitter continuo che fuma dal
                // rottame per TURWRECK_SMOKE_S (il wreck resta a mesh, sopra).
                { int nt=def_turret_count(g);
                  if(nt>TUR_DRAW_CAP) nt=TUR_DRAW_CAP;
                  for(int ti=0;ti<nt;ti++){
                      int dead=def_turret_disabled(g,ti);
                      if(dead && !gTurWasDead[ti]){ DefTurret *t=def_turret(g,ti);
                          float o[3]={t->x, ter_z(t->x,t->y)+0.5f, t->y};
                          fx_emit(&fx,o,&EXPL_SMOKE_DEF,0.0f,-1.0f);
                          fx_emit(&fx,o,&SPARK_DEF,0.0f,-1.0f);
                          fx_emit(&fx,o,&METAL_DEBRIS_DEF,0.0f,-1.0f);   // rottami radiali
                          fx_start_emitter(&fx,o,&TURWRECK_SMOKE_DEF,TURWRECK_SMOKE_S); }
                      gTurWasDead[ti]=(uint8_t)dead; } }
                // zombie a contatto d'una torretta (contact siege): il danno
                // c'era gia', ora si VEDE — latch della clip d'attacco puntata
                // all'emplacement (def_contact_turret, fresco da def_update).
                if(combat){ int an=simp_count(s); const float *apx=simp_px(s), *apy=simp_py(s);
                  for(int ai=0;ai<an;ai++){
                      int aslot=simp_slot_of(s,ai);
                      int ti=def_contact_turret(g,aslot); if(ti<0) continue;
                      DefTurret *t=def_turret(g,ti); if(!t) continue;
                      vat_layer_attack(vl,aslot,t->x-apx[ai],t->y-apy[ai]); } }
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
#ifdef GAME_SHELL
        // cinematiche heli: la camera SEGUE l'elicottero. Convergenza
        // esponenziale su posizione e zoom (HELI_CAM_HH) — parte da dove il
        // giocatore ha lasciato la camera e ci arriva progressivamente, mai
        // salti; poi resta agganciata all'heli in volo (2026-07-12).
        if(gHeli.active && !ed.active){
            // consegna: depositato il container (carrying->0) la camera molla
            // l'heli e resta a inquadrare la BASE; estrazione: segue l'heli
            // fino all'uscita.
            float tx=gHeli.x, tz=gHeli.z;
            if(gHeli.active==1 && !gHeli.carrying){ tx=gLzX; tz=gLzY; }
            float kf=1.0f-expf(-HELI_CAM_RATE*(float)frame_t);
            cx+=(tx-cx)*kf; cz+=(tz-cz)*kf;
            hh+=(HELI_CAM_HH-hh)*kf;
        }
        // modalità soldato: camera follow (decisione 2, SOLDIER_DESIGN). Stessa
        // convergenza esponenziale dell'heli — mai salti; all'uscita la camera
        // resta dov'è ed è di nuovo del pan RTS.
        if(gSolMode && gSol && soldier_active(gSol) && !ed.active && !gHeli.active
           && drag_cam!=1){
            float kf=1.0f-expf(-6.0f*(float)frame_t);
            cx+=(soldier_x(gSol)-cx)*kf; cz+=(soldier_y(gSol)-cz)*kf;
        }
        // vincoli camera di gioco (GAME_CAM_*): un solo punto di enforcement,
        // a valle di TUTTI gli input (rotella, +/-, frecce, drag RMB, tasto C)
        // — l'elevation e la prospettiva libera si toccano solo in EDIT.
        if(!ed.active){
            el=GAME_CAM_EL; cam_free=0;
            if(hh<GAME_CAM_HH_MIN)hh=GAME_CAM_HH_MIN;
            if(hh>GAME_CAM_HH_MAX)hh=GAME_CAM_HH_MAX;
        }
#endif
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
        // torrette (modello base+gun o pilastrino, rebuild ogni frame: le distrutte spariscono)
        { int turNV=build_turret_mesh(g,turBuf,turMaxV);
          if(turNV){ glBindVertexArray(turVao);glBindBuffer(GL_ARRAY_BUFFER,turVbo);
              glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)turNV*9*sizeof(float),turBuf);
              glDrawArrays(GL_TRIANGLES,0,turNV); } }
        // mine piazzate (GAME_PLAN fase D): landmine a terra per ogni trappola viva
        { int mNV=build_mine_mesh(&traps,mineBuf,mineMaxV);
          if(mNV){ glBindVertexArray(mineVao);glBindBuffer(GL_ARRAY_BUFFER,mineVbo);
              glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)mNV*9*sizeof(float),mineBuf);
              glDrawArrays(GL_TRIANGLES,0,mNV); } }
        // streak dei proiettili: comet additivo dalla bocca all'impatto (pool
        // avanzato nel loop a passo fisso). Additive, depth-test ma senza write.
        { int tv=build_tracer_mesh(trBuf);
          if(tv){ glUseProgram(trProg);glUniformMatrix4fv(uVPtr,1,GL_FALSE,vp);
              glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE); glDepthMask(GL_FALSE);
              glBindVertexArray(trVao);glBindBuffer(GL_ARRAY_BUFFER,trVbo);
              glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)tv*7*sizeof(float),trBuf);
              glLineWidth(2.5f); glDrawArrays(GL_LINES,0,tv);
              glDepthMask(GL_TRUE); glDisable(GL_BLEND); } }

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
        { int sv=build_struct_mesh(g,sc.cell,stBuf,stMaxV);
            if(sv){ glUseProgram(progFlat);glUniformMatrix4fv(uVPflat,1,GL_FALSE,vp);
                glBindVertexArray(stVao);glBindBuffer(GL_ARRAY_BUFFER,stVbo);
                glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)sv*9*sizeof(float),stBuf);
                glDrawArrays(GL_TRIANGLES,0,sv); } }

        // soldato skinned (SOLDIER_DESIGN): al posto della sagoma verde di
        // build_drag_mesh. Animazione a tempo di frame (fiction, come l'heli):
        // idle/run dalla velocità del corpo draggable; heading = mira in
        // modalità, direzione di marcia altrimenti, smussato esponenziale.
        // I blocchi successivi settano il loro program: nessuno stato ereditato.
#ifdef GAME_SHELL
        if(gSolMdlOk && gSol && soldier_active(gSol) &&
           soldier_climbing(gSol) && gSolClimb.on &&
           gSolClipClimb>=0 && gSolClipJump>=0){
            // fiction scavalcamento: la clip climb (root motion bakata) porta
            // la mesh su e oltre dal punto di partenza; il jump_down parte
            // ancorato in cima (raccordo hips fine-climb -> inizio-jump) e
            // durante la finestra di caduta l'ancora scivola a terra verso
            // l'atterraggio fisico (il drop della clip è più corto del muro).
            // I metri delle clip arrivano al mondo attraverso root_pre (0.01
            // dell'Armature FBX) *e* l'auto-scala: il fattore è il prodotto
            // (= 1.0 col default VAT_HORDE_SOL_H su rig alto 1.8).
            const float *rp=gSolMdl.skeleton.root_pre;
            float s_=gSolMdlScale*sqrtf(rp[0]*rp[0]+rp[1]*rp[1]+rp[2]*rp[2]);
            float dxn=sinf(gSolClimb.yaw), dyn=cosf(gSolClimb.yaw);
            float dc=gSolMdl.clips[gSolClipClimb].duration;
            float t=gSolClimb.t, ax,az_, ayoff=0.0f; int clip;
            if(t<dc){
                clip=gSolClipClimb; ax=gSolClimb.x0; az_=gSolClimb.y0;
            } else {
                clip=gSolClipJump; float tj=t-dc;
                float f=(tj-SOL_JUMP_T0)/(SOL_JUMP_T1-SOL_JUMP_T0);
                if(f<0.0f)f=0.0f; if(f>1.0f)f=1.0f;
                float bx=gSolClimb.x0+dxn*SOL_CLIMB_FWD*s_,
                      bz=gSolClimb.y0+dyn*SOL_CLIMB_FWD*s_;
                float lx=gSolClimb.ex-dxn*SOL_JUMP_FWD*s_,
                      lz=gSolClimb.ey-dyn*SOL_JUMP_FWD*s_;
                ax=bx+(lx-bx)*f; az_=bz+(lz-bz)*f;
                ayoff=(SOL_CLIMB_ENDHIP-SOL_JUMP_HIP0)*s_*(1.0f-f);
            }
            int fresh = gSolAnim.clip_index!=clip;
            anim_state_play(&gSolAnim,clip,false);
            if(fresh && clip==gSolClipJump){
                // niente crossfade climb->jump: fonderebbe due quote hips
                // diverse (fine climb 1.94 vs inizio jump 0.85+ancora) con un
                // pop verticale; le pose di confine sono entrambe in piedi.
                gSolAnim.blend_timer=0.0f; gSolAnim.prev_clip_index=-1;
            }
            anim_state_update(&gSolAnim,&gSolMdl,(float)frame_t);
            mat4 solmm; sol_model_mat(solmm,ax,az_,gSolClimb.yaw+gSolYawAdj);
            solmm[13]+=ayoff;
            model_render(&gSolMdl,gSkinProg,vp,solmm,&gSolAnim);
        } else if(gSolMdlOk && gSol && soldier_active(gSol)){
            int bi=soldier_body_index(gSol);
            float sx=soldier_x(gSol), sy=soldier_y(gSol), svx=0, svy=0;
            if(bi>=0 && bi<simp_drag_count(s)){
                svx=simp_drag_vx(s)[bi]; svy=simp_drag_vy(s)[bi]; }
            float sp=sqrtf(svx*svx+svy*svy), yawT=gSolYaw;
            if(gSolMode && (gSolFiring || gSolAimT>0.0f || gSolTossT>0.0f)){
                // guarda la MIRA solo sparando (o nel gesto granata, o nella
                // coda SOL_AIM_LINGER); camminando senza sparare guarda dove
                // va — run forward naturale (tweak 2026-07-19, da testare)
                float dx=gSolAimX-sx, dy=gSolAimY-sy;
                if(dx*dx+dy*dy>1e-4f) yawT=atan2f(dx,dy);
            } else if(sp>0.3f) yawT=atan2f(svx,svy);
            float dyaw=yawT-gSolYaw;
            while(dyaw> 3.14159265f) dyaw-=6.2831853f;
            while(dyaw<-3.14159265f) dyaw+=6.2831853f;
            gSolYaw+=dyaw*(1.0f-expf(-12.0f*(float)frame_t));
            if(gSolMdl.clip_count>0){
                // gambe direzionali: 8 settori da 45° dell'angolo movimento-
                // facciata (0 avanti, +90 destra, -90 sinistra, ±180 indietro);
                // il crossfade di 0.2 s assorbe i cambi al bordo dei settori.
                // Priorità: lancio granata (one-shot) > fermo che spara (fire)
                // > gambe > idle.
                int clip=gSolClipIdle, loop=1;
                if(sp>0.5f){
                    float rel=atan2f(svx,svy)-gSolYaw;
                    while(rel> 3.14159265f) rel-=6.2831853f;
                    while(rel<-3.14159265f) rel+=6.2831853f;
                    int sec=(int)floorf((rel*57.29578f+22.5f)/45.0f);
                    clip = sec==0 ? gSolClipRun :
                           sec==1 ? gSolClipFR :
                           sec==2 ? gSolClipRight :
                           sec==3 ? gSolClipBR :
                           sec==-1 ? gSolClipFL :
                           sec==-2 ? gSolClipLeft :
                           sec==-3 ? gSolClipBL :
                           gSolClipBack;              // ±4 = indietro
                } else if(gSolFiring && gSolClipFire>=0)
                    clip=gSolClipFire;
                if(gSolTossT>0.0f && gSolClipToss>=0){
                    // il timer lo decrementa il TICK; qui solo la fiction.
                    // Al cambio clip si salta il rituale iniziale (SOL_TOSS_START)
                    int fresh = gSolAnim.clip_index!=gSolClipToss;
                    clip=gSolClipToss; loop=0;
                    anim_state_play(&gSolAnim, clip, false);
                    if(fresh) gSolAnim.time=SOL_TOSS_START;
                } else
                    anim_state_play(&gSolAnim, clip, loop!=0);
                anim_state_update(&gSolAnim,&gSolMdl,(float)frame_t);
            }
            mat4 solmm; sol_model_mat(solmm,sx,sy,gSolYaw+gSolYawAdj);
            model_render(&gSolMdl,gSkinProg,vp,solmm,
                         gSolMdl.clip_count>0?&gSolAnim:NULL);
            // mitra in mano (v. statics): posizione = bone RightHand in
            // world (LeftHand durante il toss: la destra lancia), canna
            // orizzontale sull'heading; aggiorna gGunMuz per tracer+vampa.
            int ghb = (gSolTossT>0.0f && gSolHandBoneL>=0) ? gSolHandBoneL
                                                           : gSolHandBone;
            if(gGunVbo && ghb>=0 && gSolMdl.clip_count>0){
                MdlMat4 hg; anim_bone_global(&gSolAnim,&gSolMdl,ghb,hg);
                float hx=hg[12], hy=hg[13], hz=hg[14];
                float gx=solmm[0]*hx+solmm[4]*hy+solmm[8]*hz+solmm[12];
                float gy=solmm[1]*hx+solmm[5]*hy+solmm[9]*hz+solmm[13];
                float gz=solmm[2]*hx+solmm[6]*hy+solmm[10]*hz+solmm[14];
                float ca=sinf(gSolYaw), sa=cosf(gSolYaw);  // +X locale -> heading
                for(int k=0;k<gGunNV;k++){
                    const float *i=gGunVerts+k*9; float *o=gGunXf+k*9;
                    o[0]=gx + i[0]*ca - i[2]*sa;
                    o[1]=gy + i[1];
                    o[2]=gz + i[0]*sa + i[2]*ca;
                    o[3]=i[3]*ca - i[5]*sa; o[4]=i[4]; o[5]=i[3]*sa + i[5]*ca;
                    o[6]=i[6]; o[7]=i[7]; o[8]=i[8];
                }
                glUseProgram(progFlat);glUniformMatrix4fv(uVPflat,1,GL_FALSE,vp);
                glBindVertexArray(gGunVao);glBindBuffer(GL_ARRAY_BUFFER,gGunVbo);
                glBufferSubData(GL_ARRAY_BUFFER,0,
                                (GLsizeiptr)gGunNV*9*sizeof(float),gGunXf);
                glDrawArrays(GL_TRIANGLES,0,gGunNV);
                glBindVertexArray(0);
                gGunMuz[0]=gx+GUN_MUZ_X*ca; gGunMuz[1]=gy;
                gGunMuz[2]=gz+GUN_MUZ_X*sa;
                gGunMuzOn=1;
            } else gGunMuzOn=0;
        }
#endif

#ifdef GAME_SHELL
        // cinematiche elicottero (BASE_DESIGN §4): avanzano col tempo di frame
        // (fiction pura; in DEPLOY la sim è congelata, in EXTRACT continua a
        // girare senza combat — l'orda si muove sotto l'heli) e disegnano
        // col shader mesh texturato dei mesh-gib. Il container appeso lo ha già
        // stampato build_struct_mesh qui sopra.
        if(gHeli.active){
            heli_update((float)frame_t);
            heli_draw(mProg,uVPm,uModelm,vp);
        }
#endif

        // crateri scorch persistenti (§7 v1): dischi scuri col shader dei decal,
        // SOTTO il sangue (disegnati prima) -> gli schizzi coprono il cratere.
        { int ns=gScorchN;
          for(int i=0;i<ns;i++){ float *e=&gScorch[i*3],*o=&scorchinst[i*7];
              o[0]=e[0]; o[1]=ter_z(e[0],e[1])+0.02f; o[2]=e[1];
              o[3]=e[2]; o[4]=0.09f; o[5]=0.06f; o[6]=0.05f; }   // nero-marrone
          if(ns){ glUseProgram(progDc);glUniformMatrix4fv(uVPdc,1,GL_FALSE,vp);
              glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDepthMask(GL_FALSE);
              glBindVertexArray(dcVao);glBindBuffer(GL_ARRAY_BUFFER,dcInst);
              glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)ns*7*sizeof(float),scorchinst);
              glDrawArraysInstanced(GL_TRIANGLE_FAN,0,SHADN,ns);
              glDepthMask(GL_TRUE);glDisable(GL_BLEND); } }

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

        // sagome-cadavere persistenti (sopra il sangue, sotto l'orda) = RING
        // IMPOSTOR (CORPSE_DESIGN §10.8): billboard nel piano di vista, vista da
        // (az camera − heading), crossfade 2 viste, relight per heading nell'FS.
        // Ancora a TERRA (la quota del centro-bake la aggiunge il VS). Blended,
        // no depth write: i corpi (bassi) non occludono mai i vivi.
        { int nq=vat_layer_fill_corpse_decals(vl,cdecraw,CORPSEDECMAX);
          for(int i=0;i<nq;i++){ float *r=cdecraw+i*6,*o=cdecinst+i*7;
              o[0]=r[0]; o[1]=ter_z(r[0],r[1]); o[2]=r[1]; o[3]=r[2];
              o[4]=CORPSE_HALF*r[3]; o[5]=r[4]; o[6]=r[5]; }                  // half, colonna, outfit
          if(nq){
              // basi del piano di vista: right orizzontale, up2 = right x fwd
              // (ortonormali per costruzione -> up2 gia' unitario)
              float fwd[3]={ctr[0]-eye[0],ctr[1]-eye[1],ctr[2]-eye[2]};
              float fl=sqrtf(fwd[0]*fwd[0]+fwd[1]*fwd[1]+fwd[2]*fwd[2]);
              fwd[0]/=fl; fwd[1]/=fl; fwd[2]/=fl;
              float rgt[3]={cosf(az),0.0f,-sinf(az)};
              float up2[3]={ rgt[1]*fwd[2]-rgt[2]*fwd[1],
                             rgt[2]*fwd[0]-rgt[0]*fwd[2],
                             rgt[0]*fwd[1]-rgt[1]*fwd[0] };
              glUseProgram(cdProg);glUniformMatrix4fv(uVPcd,1,GL_FALSE,vp);
              glUniform3f(uRightCd,rgt[0],rgt[1],rgt[2]);
              glUniform3f(uUp2Cd,up2[0],up2[1],up2[2]);
              glUniform3f(uToCamCd,-fwd[0],-fwd[1],-fwd[2]);
              glUniform1f(uLookYCd,RING_LOOKY); glUniform1f(uCamAzCd,az);
              glUniform1f(uNColsCd,(float)corpseNCols); glUniform1f(uNViewCd,(float)RINGVIEWS);
              glUniform1f(uNOutCd,(float)nOutRing);
              glUniform1i(uBlendCd,1);                                  // crossfade sempre in gioco
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
            if(gTerOn) for(int q=0;q<count;q++) inst[q*14+1]+=ter_z(inst[q*14+0],inst[q*14+2]);
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

        // ghost di piazzamento (PLAY): disco verde se valido / rosso se no, al cursore.
        // Depth off → sempre visibile sopra l'orda. Riusa progFlat + il VBO overlay.
        if(plc.active && !ed.active){
#ifdef GAME_SHELL
            // soldato-costruttore: il ghost segue LUI anche senza eventi mouse,
            // e il cono d'anteprima mostra il facing che verrà committato (mira)
            if(sol_builder()){ float bx,by; sol_build_spot(&bx,&by);
                               pl_set_cursor(&plc,bx,by);
                               gTAimFacing=atan2f(by-soldier_y(gSol),
                                                  bx-soldier_x(gSol)); }
#endif
            const PlItem *it=pl_selected(&plc);
            int ov=0;
#ifdef GAME_SHELL
            gLineOn=0;
            if(it && it->kind==PL_BARRICADE && plineOn){
                // ghost di LINEA (§5): i moduli previsti + costo live (la barra
                // lo disegna accanto al cursore leggendo gLine*).
                PlLinePlan lp;
                int okl=pl_line_validate(&plc,g,s,plineAx,plineAy,plc.cx,plc.cy,&lp);
                float cr=okl?0.20f:0.95f, cg=okl?0.90f:0.18f;
                float ca=cosf(lp.ang), sa=sinf(lp.ang);
                for(int m=0;m<lp.nmod && ov+6<=EDOVL_MAXV;m++){
                    float hx=0.5f*lp.mlen[m]-0.06f;         // gap: si leggono i moduli
                    ov=ed_push_bar(edovl,ov,lp.mx[m]-ca*hx,lp.my[m]-sa*hx,
                                   lp.mx[m]+ca*hx,lp.my[m]+sa*hx,it->h,cr,cg,0.18f);
                }
                if(!ov) ov=ed_push_marker(edovl,0,plc.cx,plc.cy,0.6f,cr,cg,0.18f);
                gLineOn=1; gLineValid=okl; gLineCost=lp.cost; gLineLen=lp.len;
            } else
#endif
            {
                // durante il gesto di mira il ghost resta inchiodato all'ANCORA
                // (il cursore è a fine drag): posizione e validate vanno lì.
                float ax=plc.cx, ay=plc.cy;
                if(gTAimOn && it && it->kind==PL_TURRET){
                    ax=gTAimX; ay=gTAimY;
                    float dx=plc.cx-ax, dy=plc.cy-ay;
                    if(dx*dx+dy*dy>=0.09f) gTAimFacing=atan2f(dy,dx);
                    float sx=plc.cx, sy=plc.cy;
                    pl_set_cursor(&plc,ax,ay); pl_validate(&plc,g,s);
                    pl_set_cursor(&plc,sx,sy);
                } else pl_validate(&plc,g,s);               // colore fresco ogni frame
                float r = it ? fmaxf(0.5f, 0.5f*fmaxf(it->w,fmaxf(it->h,it->radius*2.0f))) : 0.6f;
                float cr = plc.valid?0.20f:0.95f, cg = plc.valid?0.90f:0.18f, cb=0.18f;
                ov = ed_push_marker(edovl,0,ax,ay,r, cr,cg,cb);
                if(it && it->kind==PL_TURRET){
                    // cono di mira (facing±half): segue il drag; prima del
                    // gesto mostra l'ultima direzione usata. Raggio = gittata
                    // (default per tipo, allineato a place.c commit_turret).
                    float R=it->range;
                    if(R<=0.0f) R=(it->heavy==2)?12.0f:(it->heavy==3)?18.0f:40.0f;
                    float half=(it->arc_deg>0.0f?it->arc_deg:90.0f)
                               *(3.14159265f/360.0f);
                    ov=push_aim_cone(edovl,ov,EDOVL_MAXV,ax,ay,
                                     gTAimFacing,half,R,cr,cg,cb);
                }
            }
            if(ov){ glDisable(GL_DEPTH_TEST);
                glUseProgram(progFlat);glUniformMatrix4fv(uVPflat,1,GL_FALSE,vp);
                glBindVertexArray(ovVao);glBindBuffer(GL_ARRAY_BUFFER,ovVbo);
                glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)ov*9*sizeof(float),edovl);
                glDrawArrays(GL_TRIANGLES,0,ov);
                glEnable(GL_DEPTH_TEST); }
        }
        // rotte previste + marker exit (LOOP_DESIGN B, decisione 4): SOLO in
        // PREP ogni exit mostra bordo + streamline animata lungo il flow
        // field — piazzi un muro, il commit nav sposta la rotta davanti agli
        // occhi. In assalto niente overlay (verdetto 2026-07-16: l'orda
        // stessa mostra rotte e provenienza, il rettangolo è rumore).
        if(!ed.active && sc.n_exit>0){
            int prep_on=0;
#ifdef GAME_SHELL
            prep_on=(gApp.state==APP_PREP);
#else
            prep_on = gMissionOn && mission_state(&gMission)==MISSION_PREP;
#endif
            // LOOP A: durante l'ANNUNCIO le exit dell'ondata in arrivo
            // lampeggiano (bordo che pulsa). Niente streamline in assalto
            // (decisione 4): solo il "da dove", il resto lo mostra l'orda.
            MissionWaveInfo pwi;
            if(!prep_on && gMissionOn && mission_wave_pending(&gMission,&pwi)){
                gRouteT+=(float)frame_t;
                float pulse=0.35f+0.65f*(0.5f+0.5f*sinf(gRouteT*6.0f));
                int ov=0;
                for(int k=0;k<pwi.n;k++){
                    if(pwi.e[k].exit_idx>=sc.n_exit || ov+16>EDOVL_MAXV) break;
                    const SceneExit *ex=&sc.exits[pwi.e[k].exit_idx];
                    float ex1=ex->x+ex->w, ey1=ex->y+ex->h;
                    float r=0.98f*pulse, gg=0.22f*pulse, b=0.10f*pulse;
                    ov=ed_push_bar(edovl,ov,ex->x,ex->y,ex1,ex->y,0.45f,r,gg,b);
                    ov=ed_push_bar(edovl,ov,ex1,ex->y,ex1,ey1,0.45f,r,gg,b);
                    ov=ed_push_bar(edovl,ov,ex1,ey1,ex->x,ey1,0.45f,r,gg,b);
                    ov=ed_push_bar(edovl,ov,ex->x,ey1,ex->x,ex->y,0.45f,r,gg,b);
                }
                if(ov){ glDisable(GL_DEPTH_TEST);
                    glUseProgram(progFlat);glUniformMatrix4fv(uVPflat,1,GL_FALSE,vp);
                    glBindVertexArray(ovVao);glBindBuffer(GL_ARRAY_BUFFER,ovVbo);
                    glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)ov*9*sizeof(float),edovl);
                    glDrawArrays(GL_TRIANGLES,0,ov);
                    glEnable(GL_DEPTH_TEST); }
            }
            if(prep_on){
                gRouteT+=(float)frame_t;
                float phase=fmodf(gRouteT*3.0f,ROUTE_GAP);   // ~3 m/s verso il goal
                int ov=0;
                for(int ei=0; ei<sc.n_exit; ei++){
                    const SceneExit *ex=&sc.exits[ei];
                    float ex1=ex->x+ex->w, ey1=ex->y+ex->h;
                    if(ov+24>EDOVL_MAXV) break;
                    // bordo del rect exit (rosso)
                    ov=ed_push_bar(edovl,ov,ex->x,ex->y,ex1,ex->y,0.30f,0.95f,0.20f,0.15f);
                    ov=ed_push_bar(edovl,ov,ex1,ex->y,ex1,ey1,0.30f,0.95f,0.20f,0.15f);
                    ov=ed_push_bar(edovl,ov,ex1,ey1,ex->x,ey1,0.30f,0.95f,0.20f,0.15f);
                    ov=ed_push_bar(edovl,ov,ex->x,ey1,ex->x,ex->y,0.30f,0.95f,0.20f,0.15f);
                    ov=push_route_dashes(edovl,ov,EDOVL_MAXV,s,
                                         ex->x+0.5f*ex->w,ex->y+0.5f*ex->h,
                                         sc.world_w,sc.world_h,phase,
                                         0.95f,0.38f,0.12f);
                }
                if(ov){ glDisable(GL_DEPTH_TEST);
                    glUseProgram(progFlat);glUniformMatrix4fv(uVPflat,1,GL_FALSE,vp);
                    glBindVertexArray(ovVao);glBindBuffer(GL_ARRAY_BUFFER,ovVbo);
                    glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)ov*9*sizeof(float),edovl);
                    glDrawArrays(GL_TRIANGLES,0,ov);
                    glEnable(GL_DEPTH_TEST); }
            }
        }
#ifdef GAME_SHELL
        // X di mira del mortaio: due barre incrociate sul punto mirato (aiming
        // attivo in ASSALTO). Riusa l'overlay flat del ghost di placement.
        // Fuori gittata (min/max) la X si spegne in grigio; i due anelli di
        // gittata attorno alla base restano visibili per tutto l'aiming.
        if(gAimMort && gApp.state==APP_ASSAULT && !ed.active){
            int ov=0; float sX=1.4f;
            float rdx=gAimX-gBaseOX, rdy=gAimY-gBaseOY;
            float rd=sqrtf(rdx*rdx+rdy*rdy);
            int okr=(rd>=gMortMinR && rd<=gMortMaxR && gMortCd<=0.0f);  // cd: X grigia
            float xr=okr?0.95f:0.45f, xg=okr?0.25f:0.45f, xb=okr?0.12f:0.50f;
            ov=ed_push_bar(edovl,ov, gAimX-sX,gAimY-sX, gAimX+sX,gAimY+sX, 0.35f, xr,xg,xb);
            ov=ed_push_bar(edovl,ov, gAimX-sX,gAimY+sX, gAimX+sX,gAimY-sX, 0.35f, xr,xg,xb);
            const int NSEG=72;
            for(int ring=0;ring<2;ring++){
                float R=ring?gMortMaxR:gMortMinR;
                for(int k=0;k<NSEG && ov+6<=EDOVL_MAXV;k++){
                    float a0=(float)k*6.2831853f/NSEG, a1=(float)(k+1)*6.2831853f/NSEG;
                    ov=ed_push_bar(edovl,ov, gBaseOX+cosf(a0)*R,gBaseOY+sinf(a0)*R,
                                   gBaseOX+cosf(a1)*R,gBaseOY+sinf(a1)*R, 0.3f,
                                   0.95f,0.45f,0.12f);
                }
            }
            if(ov){ glDisable(GL_DEPTH_TEST);
                glUseProgram(progFlat);glUniformMatrix4fv(uVPflat,1,GL_FALSE,vp);
                glBindVertexArray(ovVao);glBindBuffer(GL_ARRAY_BUFFER,ovVbo);
                glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)ov*9*sizeof(float),edovl);
                glDrawArrays(GL_TRIANGLES,0,ov);
                glEnable(GL_DEPTH_TEST); }
        }
        // hover inspect: risolve l'elemento sotto il cursore (tooltip disegnato
        // da shell_build_ui). Solo in PREP/ASSALTO, mouse sul MONDO (sopra le
        // barre), niente in mira mortaio o durante un drag di camera. Scansione
        // dei piani di quota dall'alto verso terra: il cursore prende la
        // struttura su tutta la sagoma (facciata e tetto), non solo alla base.
        // (Approssimazione: quote assolute — su terreni collinari il bordo alto
        // può slittare di poco, per un tooltip è irrilevante.)
        gHovOn=0; gHovTurret=-1; gHovSid=-1;
        if(!ed.active && (gApp.state==APP_PREP || gApp.state==APP_ASSAULT) &&
           !drag_cam && !(gAimMort && gApp.state==APP_ASSAULT)){
            if(gApp.state==APP_PREP) prep_ui_layout(SW,SH); else strikes_ui_layout(SW,SH);
            float barY=(gApp.state==APP_PREP)?gPrepBarY:gStrikeBarY;
            float r0[3],r1[3];
            if(mouse_py<barY && pick_ray(vp,mouse_px,mouse_py,SW,SH,r0,r1)){
                for(float hplane=HOVER_SCAN_TOP; hplane>=0.0f; hplane-=0.4f){
                    float wx,wy;
                    if(pick_ray_plane(r0,r1,hplane,&wx,&wy) &&
                       hover_resolve(&sc,g,wx,wy,hplane)) break;
                }
            }
        }
        // (niente cono di mira su hover: l'indicazione di direzione appartiene
        // solo al gesto di piazzamento e alla modalità REGOLA — tolto su
        // richiesta 2026-07-12. Il tooltip hover resta; gHovTurret resta
        // risolto per i click di ricarica/riparazione e per REGOLA.)
        // REGOLA (BIOMASS §4): col cono in mano la direzione insegue il cursore
        // (commit al rilascio, eventi); a mani vuote il cono della torretta sotto
        // il mouse dice cosa si sta per prendere. Stesso overlay flat del ghost
        // di piazzamento, stessa lettura visiva.
        if(gAdjOn && !ed.active &&
           (gApp.state==APP_PREP || gApp.state==APP_ASSAULT)){
            int tid = (gAdjTid>=0) ? gAdjTid : gHovTurret;
            if(tid>=0){
                DefTurret *t=def_turret(g,tid);
                float half=(t->arc_max-t->arc_min)*0.5f;
                if(half<0.05f) half=0.05f;
                float facing=t->ang;
                if(gAdjTid>=0){                    // drag: la direzione è il cursore
                    float wx,wy;
                    if(pick_y0(vp,mouse_px,mouse_py,SW,SH,&wx,&wy)){
                        float dx=wx-t->x, dy=wy-t->y;
                        if(dx*dx+dy*dy>=0.09f) gAdjFacing=atan2f(dy,dx);
                    }
                    facing=gAdjFacing;
                }
                float live=(gAdjTid>=0)?1.0f:0.5f;   // in mano acceso, in hover smorto
                int ov=push_aim_cone(edovl,0,EDOVL_MAXV,t->x,t->y,facing,half,
                                     t->range, 0.95f*live,0.75f*live,0.20f*live);
                if(ov){ glDisable(GL_DEPTH_TEST);
                    glUseProgram(progFlat);glUniformMatrix4fv(uVPflat,1,GL_FALSE,vp);
                    glBindVertexArray(ovVao);glBindBuffer(GL_ARRAY_BUFFER,ovVbo);
                    glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)ov*9*sizeof(float),edovl);
                    glDrawArrays(GL_TRIANGLES,0,ov);
                    glEnable(GL_DEPTH_TEST); }
            }
        }
        // barre di reload world-space (BIOMASS §5): solo sulle torrette che stanno
        // ricaricando (informazione transitoria e ACTIONABLE — la decisione
        // 2026-07-08 contro le barre HP permanenti resta valida). Qui si proiettano
        // i punti sopra la canna; il disegno è nel layer UI (shell_build_ui).
        gRelBarN=0;
        if(!ed.active && (gApp.state==APP_PREP || gApp.state==APP_ASSAULT)){
            int nt=def_turret_count(g);
            for(int i=0;i<nt && gRelBarN<REL_BAR_MAX;i++){
                float left=def_turret_reloading(g,i);
                if(left<=0.0f || def_turret_disabled(g,i)) continue;
                DefTurret *t=def_turret(g,i);
                float rs=(t->reload_s>0.0f)?t->reload_s:gReloadDef;
                float sx,sy;
                if(!world_to_screen(vp,t->x,ter_z(t->x,t->y)+2.4f,t->y,SW,SH,&sx,&sy))
                    continue;                       // dietro la camera
                gRelBar[gRelBarN].x=sx; gRelBar[gRelBarN].y=sy;
                gRelBar[gRelBarN].f=1.0f-left/rs;   // 0 = appena scattata, 1 = pronta
                gRelBarN++;
            }
        }
        // barra HP del soldato (SOLDIER_DESIGN): sopra la testa, solo in modalità
        gSolBarOn=0;
        if(!ed.active && gSolMode && gSol && soldier_active(gSol)){
            float sx,sy, wx=soldier_x(gSol), wy=soldier_y(gSol);
            if(world_to_screen(vp,wx,ter_z(wx,wy)+2.2f,wy,SW,SH,&sx,&sy)){
                gSolBarX=sx; gSolBarY=sy;
                gSolBarF=soldier_hp(gSol)/soldier_hp_max(gSol);
                gSolBarOn=1;
            }
        }
        // overlay shell (title/menu/briefing/debrief + barra di fase): 2D in
        // pixel, sopra tutto, blend alpha (l'alpha viaggia in aNormal.x).
        gUiV=0; gUiImgN=0; shell_build_ui(SW,SH,g,mouse_px,mouse_py);
        if(gUiV>0 || gUiImgN>0){
            glDisable(GL_DEPTH_TEST); glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
            mat4 uivp; m_ortho(uivp,0.0f,(float)SW,(float)SH,0.0f,-1.0f,1.0f);
            if(gUiImgN>0){                      // immagini sotto quad/testo
                glUseProgram(imgProg); glUniformMatrix4fv(uVPimg,1,GL_FALSE,uivp);
                glActiveTexture(GL_TEXTURE0);
                glBindVertexArray(imgVao); glBindBuffer(GL_ARRAY_BUFFER,imgVbo);
                for(int i=0;i<gUiImgN;i++){
                    float x=gUiImg[i].x,y=gUiImg[i].y,w=gUiImg[i].w,h=gUiImg[i].h;
                    float r=gUiImg[i].r,gg=gUiImg[i].g,b=gUiImg[i].b,a=gUiImg[i].a;
                    float v[6][5]={{x,y,0,0,0},{x+w,y,0,1,0},{x+w,y+h,0,1,1},
                                   {x,y,0,0,0},{x+w,y+h,0,1,1},{x,y+h,0,0,1}};
                    float buf[6*9];
                    for(int k=0;k<6;k++){ float *o=buf+k*9;
                        o[0]=v[k][0];o[1]=v[k][1];o[2]=v[k][2];
                        o[3]=a;o[4]=v[k][3];o[5]=v[k][4]; o[6]=r;o[7]=gg;o[8]=b; }
                    glBindTexture(GL_TEXTURE_2D,gUiImg[i].tex);
                    glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)sizeof buf,buf);
                    glDrawArrays(GL_TRIANGLES,0,6);
                }
            }
            if(gUiV>0){
                glUseProgram(uiProg); glUniformMatrix4fv(uVPui,1,GL_FALSE,uivp);
                glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,gUiFont.tex);
                glBindVertexArray(uiVao); glBindBuffer(GL_ARRAY_BUFFER,uiVbo);
                glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)gUiV*9*sizeof(float),gUiBuf);
                glDrawArrays(GL_TRIANGLES,0,gUiV);
            }
            glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
        }
#endif
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
            else if(gLzCore>=0){ int pc=(int)(100.0f*def_struct_hp(g,gLzCore)/(def_struct_hp_max(g,gLzCore)+1e-3f));
                snprintf(base,sizeof base, def_lost(g)?" | LZ PERSA":" | LZ %d%%", pc<0?0:pc); }
            else if(gStructOn){ int ns=def_struct_count(g),up=0; for(int q=0;q<ns;q++) if(!def_struct_collapsed(g,q)) up++;
                snprintf(base,sizeof base," | mura %d/%d", up, ns); }
            char wv[128]="";
            if(dir) snprintf(wv,sizeof wv," | ondata %d budget %d",def_director_wave(dir),def_budget(g));
            else if(gMissionOn){                        // fase A: fase/timer/pool
                float tl=mission_time_left(&gMission);
                char tls[16]=""; if(tl>=0.0f) snprintf(tls,sizeof tls," %.0fs",(double)tl);
                char pls[24]="";
                if(mission_pool(&gMission)>0) snprintf(pls,sizeof pls," %d/%d",
                    mission_emitted(&gMission),mission_pool(&gMission));
                const char *mst = gMission.state==MISSION_PREP?"PREP (INVIO=via)":
                                  gMission.state==MISSION_ASSAULT?"ASSALTO":
                                  gMission.state==MISSION_WON?"VINTA":"PERSA";
                char ws[40]="";                    // LOOP A: k/N (+countdown)
                if(mission_wave_total(&gMission)>0){
                    MissionWaveInfo pwi;
                    if(mission_wave_pending(&gMission,&pwi))
                        snprintf(ws,sizeof ws," ondata %d/%d tra %.0fs (INVIO=subito)",
                                 pwi.index,pwi.total,(double)pwi.countdown);
                    else snprintf(ws,sizeof ws," ondata %d/%d",
                                  mission_wave_current(&gMission),
                                  mission_wave_total(&gMission));
                }
                snprintf(wv,sizeof wv," | %s%s%s%s budget %d",mst,tls,pls,ws,def_budget(g)); }
            char pl[96]="";
            if(plc.active){ const PlItem *it=pl_selected(&plc);
                const char *why = plc.reason==PL_NOFUNDS?"$":plc.reason==PL_BLOCKED?"statico":plc.reason==PL_OVERLAP?"occupato":"ok";
                snprintf(pl,sizeof pl," | PLACE:%s($%d) rot%d %s budget%d ([]=voce ,.=ruota LMB=piazza RMB=esci)",
                         it?it->name:"-", it?it->cost:0, plc.rot90, why, def_budget(g)); }
            snprintf(title,sizeof title,"vat_horde — %d agenti%s%s | kills %d crawler %d%s | sim %.2f ren %.2f ms | %.0f fps",
                     total,wv,pl,def_kills(g),def_count_wound(g,DW_CRAWLING),base,S,R,1000.0/(S+L+R));
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
#ifdef GAME_SHELL
        // watcher musica: appena si esce dall'ASSAULT (vittoria, sconfitta o
        // ESC verso il menu) ferma la traccia di battaglia. Un solo punto,
        // dopo input E sim-update, cattura tutte le uscite dallo stato.
        { static AppState music_prev=APP_TITLE;
          if(gApp.state!=APP_ASSAULT && music_prev==APP_ASSAULT) au_music_stop();
          music_prev=gApp.state; }
#endif
        SDL_GL_SwapWindow(win);
    }
    free(stBuf); free(turBuf); free(dragBuf); if(dir) def_director_destroy(dir);
    au_shutdown();
#ifdef GAME_SHELL
    free(gUiBuf);
#endif
    if(gTerOn) terrain_free(&gTer);
    def_destroy(g); vat_layer_destroy(vl); simp_destroy(s); scene_free(&sc);
    SDL_GL_DestroyContext(ctx);SDL_DestroyWindow(win);SDL_Quit(); return 0;
}
