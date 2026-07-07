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
static TurretModel gTurM[2];                        // [0]=light, [1]=heavy
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
// veto editor (§10): non si piazza nulla su una cella-statico (buco palazzo).
static int ter_blocked(float x, float y){ return gTerOn && terrain_hole(&gTer, x, y); }

// --- piazzamento a runtime (PLACEMENT_DESIGN.md): catalogo + veto static ---
static int pl_blocked_host(void *u, float x, float y){ (void)u; return ter_blocked(x,y); }
static const PlItem PL_CAT[] = {
    /* kind        name          cost   w     h    radius  hp     mass   combat: 0 = default | trap: 0 */
    { PL_BARRICADE, "Barricata",   50,  4.0f, 1.0f, 0.0f, 300.0f, 30.0f, 0, 0,0,0, 0, 0,0,0,0,0,0 },  /* mass>0 = detriti al crollo */
    { PL_TURRET,    "Torretta",   100,  1.0f, 1.0f, 0.5f,   0.0f,  0.0f, 0, 0,0,0, 0, 0,0,0,0,0,0 },
    { PL_BIN,       "Cassonetto",  20,  0.0f, 0.0f, 0.6f,   0.0f, 12.0f, 0, 0,0,0, 0, 0,0,0,0,0,0 },
    { PL_CAR,       "Auto",        60,  3.0f, 0.0f, 0.6f,   0.0f, 20.0f, 0, 0,0,0, 0, 0,0,0,0,0,0 },
};
#define PL_NCAT ((int)(sizeof(PL_CAT)/sizeof(PL_CAT[0])))

#ifdef GAME_SHELL
// Catalogo v1 del GAME (PREP_UI_DESIGN §2): niente bin/auto (entità di livello,
// §9), barriere prezzate AL METRO (strumento a linea, place.h) con spessore
// 1.0 m (>= 2 celle: niente pinch diagonali nella scala rasterizzata). La
// cancellata è il cover semi-trasparente dell'asse C: opacità 0.3, le torrette
// sparano attraverso (test_cover). Parametri combat a 0 = default di place.c.
static const PlItem PL_CAT_GAME[] = {
    /* kind        name          cost   w     h    radius  hp     mass   heavy range per dmg  opac  trap: trig blastR dmg str up arm */
    { PL_TURRET,    "Leggera",    100,  1.0f, 1.0f, 0.5f,   0.0f,  0.0f, 0,    0,0,0,         0,    0,0,0,0,0,0 },
    { PL_TURRET,    "Pesante",    250,  1.0f, 1.0f, 0.5f,   0.0f,  0.0f, 1,    0,0,0,         0,    0,0,0,0,0,0 },
    { PL_BARRICADE, "Barricata",   12,  2.5f, 1.0f, 0.0f, 300.0f, 30.0f, 0,    0,0,0,         0,    0,0,0,0,0,0 },
    { PL_BARRICADE, "Cancellata",  20,  2.5f, 1.0f, 0.0f, 200.0f, 15.0f, 0,    0,0,0,         0.3f, 0,0,0,0,0,0 },
    /* MINA (GAME_PLAN fase D): one-shot a pressione. trigger 1.2 m, blast R 6 /
     * dmg 150, loft 22/0.6, arm 1 s (non ti esplode in mano al piazzamento). */
    { PL_TRAP,      "Mina",        40,  0.0f, 0.0f, 0.3f,   0.0f,  0.0f, 0,    0,0,0,         0,    1.2f,6.0f,150.0f,22.0f,0.6f,1.0f },
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
// default granata/RMB (EXPLOSION_DESIGN §10.4: R=8, D0=180); danno da caduta
// costante v1 (§3.4: il buffer landed dà solo handle, non la v d'impatto → una
// costante ragionevole, si tara a occhio nel sandbox).
#define BLAST_R    8.0f
#define BLAST_DMG  180.0f
#define FALL_DMG   25.0f

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

#ifdef GAME_SHELL
// Attacchi speciali: colpo di mortaio (GAME_PLAN fase F, v0 PLACEHOLDER). Niente
// animazione di lancio (la base non è ancora un'entità: prossimo step importante),
// niente costo biomassa (fase E), UN colpo alla volta. In ASSALTO M o l'icona in
// barra entra in aiming (una X segue il mouse a terra); LMB programma il colpo,
// che dopo MORTAR_DELAY esplode via host_blast sul punto mirato. Stato qui (serve
// a build_world per il reset); barra/aiming più sotto. La formalizzazione (modulo
// strikes.c + test + biomassa + dispersione) arriva con la gestione base / fase F.
#define MORTAR_DELAY 1.8f          // tempo di volo (s): arco lobato ben leggibile
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
#endif

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
    // FX §6: lampo + fireball al suolo, colonna di fumo poco sopra.
    float zg = ter_z(x, y);
    float of[3] = { x, zg + 0.4f, y }, os[3] = { x, zg + 0.9f, y };
    fx_emit(fx, of, &EXPL_FLASH_DEF,    0.0f, -1.0f);
    fx_emit(fx, of, &EXPL_FIREBALL_DEF, 0.0f, -1.0f);
    fx_emit(fx, os, &EXPL_SMOKE_DEF,    0.0f, -1.0f);
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

// --- mesh delle torrette: modello .glb base+gun se caricato (il gun segue
// l'angolo di mira e rincula al colpo), altrimenti pilastrino procedurale
// (arancio = leggera, rosso = pesante). Layout 9-float del flat shader
// (pos, normal, color). Rebuilt each frame into a caller buffer: a turret
// sieged to collapse (def_turret_disabled) vanishes. maxV = buffer cap in
// verts. Returns vertex count.
static AnimSys gAnim;          // envelope one-shot dei meccanismi (anim.h): rinculo torrette
static int build_turret_mesh(DefGame *g, float *buf, int maxV){
    int nt=def_turret_count(g); int c=0;
    for(int id=0;id<nt;id++){ DefTurret *t=def_turret(g,id);
        if(def_turret_disabled(g,id)) continue;            // destroyed: gone
        // rinculo: envelope lineare 1->0 di anim.c, v² per il calcio secco.
        float rec=anim_value(&gAnim,ANIM_TURRET_RECOIL,id); rec*=rec;
        float ca=cosf(t->ang), sa=sinf(t->ang);
        TurretModel *tm=&gTurM[t->heavy?1:0];
        if(tm->ok){
            if(c + tm->base.nv + tm->gun.nv > maxV) break;
            float zb=ter_z(t->x,t->y);
            // base fissa; gun sull'angolo di mira, arretrato dal rinculo
            c=tur_emit(buf,c,&tm->base, t->x,t->y, zb, 1.0f,0.0f,
                       0.42f,0.44f,0.48f);
            float k=0.20f*gTurScale*rec;
            float gr=0.95f, gg=t->heavy?0.18f:0.55f, gb=0.10f;
            c=tur_emit(buf,c,&tm->gun, t->x-k*ca, t->y-k*sa, zb, ca,sa,
                       gr,gg,gb);
            continue;
        }
        if(c+30 > maxV) break;
        float cx=t->x-0.22f*rec*ca, cz=t->y-0.22f*rec*sa, hw=0.32f;
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
static int build_struct_mesh(DefGame *g, float cell, float *buf, int maxV){
    int bx0,by0,bx1,by1;
    if(def_struct_count(g)==0 || !def_struct_bbox(g,&bx0,&by0,&bx1,&by1)) return 0;
    int c=0; float H=2.8f;
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
    // BASE container (fase 1b): il core della LZ è UN box orientato (placeholder
    // container 6,1×2,44×2,6 m) al posto dei cubetti per-cella saltati sopra.
    // Si scurisce col danno e sparisce al crollo (celle liberate da defense).
    if(gLzCore>=0 && !def_struct_collapsed(g,gLzCore) && c+30<=maxV){
        float frac=def_struct_hp(g,gLzCore)/(def_struct_hp_max(g,gLzCore)+1e-3f);
        float t=0.35f+0.65f*frac;
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
    float hp=getenv("VAT_HORDE_LZ_HP")?atof(getenv("VAT_HORDE_LZ_HP")):1500.0f;
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
    SimP *s = scene_instantiate(sc, MAXA);
    if(!s){ fprintf(stderr,"scene_instantiate fail\n"); return -1; }
    spctx->s = s; spctx->vl = vl;
    gScorchN = gScorchHead = 0;                     // crateri scorch: mondo pulito
#ifdef GAME_SHELL
    gAimMort = 0; for(int i=0;i<STRIKE_MAX;i++) gStrikes[i].on = 0;  // reset mortaio
#endif

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
    gBaseOX=bcx; gBaseOY=bcy;                 // fallback origine mortaio (il container lo sovrascrive)
    float mn = sc->world_w<sc->world_h?sc->world_w:sc->world_h;
    float TR_R = 0.22f*mn;
    def_set_budget(g, getenv("VAT_HORDE_BUDGET")?atoi(getenv("VAT_HORDE_BUDGET")):1000);
    int placed=0;
    // a "designed" scene (walls, turrets, or a declared mission) owns its turrets: place
    // ONLY the scene's (possibly zero). A legacy scene gets the demo auto-ring.
    // a mission scene is authored: it owns its turrets (the `turret` lines, or
    // none) and must NOT get the legacy demo ring, which would carpet the LZ.
    int designed = (sc->n_wall>0 || sc->n_turret>0 || sc->mission.kind != SCENE_MISSION_NONE);
    // destructible turrets: a turret becomes a 1-cell solid the horde sieges to
    // reach the goal beyond -> exposed turrets in a breached ring get assaulted
    // and silenced (def_turret_make_destructible). HP from env, 0 = indestructible.
    float turret_hp = getenv("VAT_HORDE_TURRET_HP")?atof(getenv("VAT_HORDE_TURRET_HP")):250.0f;
    // contact-siege tuning (def_set_turret_contact): 0 = keep default. Lets the
    // turrets be made tougher/weaker to the swarm at a glance, HP unchanged.
    // Reach di gioco 2.0 m (default defense 0.9): chi passa nel corridoio
    // adiacente morde la torretta — insieme al lure sotto, le torrette nel
    // flusso si pagano (2026-07-04).
    def_set_turret_contact(g,
        getenv("VAT_HORDE_TURRET_DPS")?atof(getenv("VAT_HORDE_TURRET_DPS")):0.0f,
        getenv("VAT_HORDE_TURRET_REACH")?atof(getenv("VAT_HORDE_TURRET_REACH")):2.0f);
    // richiamo da fuoco (def_set_fire_lure): le torrette che sparano ATTIRANO
    // l'orda (rumore) — cost_user negativo attorno finché sparano, rimozione
    // esatta al silenzio/crollo. VAT_HORDE_LURE="w,r,linger" (w>=0 = off).
    { float lw=-0.5f, lr=6.0f, ll=2.5f;
      if(getenv("VAT_HORDE_LURE")) sscanf(getenv("VAT_HORDE_LURE"),"%f,%f,%f",&lw,&lr,&ll);
      def_set_fire_lure(g,lw,lr,ll); }
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
        if(sc->has_lz) build_lz_core(g,s,sc);
        char pool[24]="inf";
        if(gMission.pool_total>0) snprintf(pool,sizeof pool,"%d",gMission.pool_total);
        printf("missione: %s %.0fs prep %s, %d exit (pool %s), budget %d%s\n",
               gMission.kind==SCENE_MISSION_SURVIVE?"SURVIVE":"CLEAR",
               (double)gMission.survive_s,
               gMission.prep_s>0.0f?"a tempo":"illimitata (INVIO=via)",
               gMission.ndir, pool, def_budget(g), sc->has_lz?" + LZ":"");
    }

    DefRect drects[16]; int ndr=sc->n_spawn<16?sc->n_spawn:16;
    for(int k=0;k<ndr;k++){ drects[k].x=sc->spawn[k].x; drects[k].y=sc->spawn[k].y;
        drects[k].w=sc->spawn[k].w; drects[k].h=sc->spawn[k].h; }
    DefDirector *dir=NULL;
    if(!gMissionOn && !fillN && ndr>0){ DefDirectorCfg dc={0};
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

#ifdef GAME_SHELL
// ============================ GAME SHELL =================================
// (GAME_APP_DESIGN.md §4) Il flusso vive in app.c (logica pura, testata da
// test_app); qui c'è solo l'ESECUZIONE delle azioni (caricare il livello,
// avviare le fasi, salvare) e il disegno degli overlay con font8.
static App   gApp;
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
    float *cam_x, *cam_z; int *running;
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
        default: break;
    }
}

// ---- overlay 2D: quad + testo font8 nel flat.vs + ui.fs (alpha in aNormal.x)
#define UI_MAX_V 120000
static float *gUiBuf=NULL; static int gUiV=0;
static void ui_quad(float x,float y,float w,float h,float r,float g,float b,float a){
    if(gUiV+6>UI_MAX_V) return;
    float v[6][2]={{x,y},{x+w,y},{x+w,y+h},{x,y},{x+w,y+h},{x,y+h}};
    for(int i=0;i<6;i++){ float *o=gUiBuf+(size_t)(gUiV+i)*9;
        o[0]=v[i][0];o[1]=v[i][1];o[2]=0.0f; o[3]=a;o[4]=0;o[5]=0; o[6]=r;o[7]=g;o[8]=b; }
    gUiV+=6;
}
static void ui_text(float x,float y,float s,const char*str,float r,float g,float b,float a){
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
    int n=0,best=0;
    for(const char*p=str;*p;p++){ if(*p=='\n'){ if(n>best)best=n; n=0; } else n++; }
    if(n>best)best=n;
    return (float)best*s*FONT8_ADV;
}
static void ui_text_c(float cxpx,float y,float s,const char*str,float r,float g,float b,float a){
    ui_text(cxpx-ui_text_w(s,str)*0.5f,y,s,str,r,g,b,a);
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
        float r=it->heavy?0.85f:0.55f, g=it->heavy?0.45f:0.75f, b=it->heavy?0.20f:0.90f;
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
        float rng=it->range>0?it->range:40.0f;
        float per=it->fire_period>0?it->fire_period:(it->heavy?0.5f:0.12f);
        snprintf(l1,n1,"%s - %d$",it->heavy?"TORRETTA PESANTE":"TORRETTA LEGGERA",it->cost);
        snprintf(l2,n2,"GITTATA %.0f M - %.1f COLPI/S%s",(double)rng,1.0/per,
                 it->heavy?" - SMEMBRA":"");
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
    if(gLineOn){                                    // drag di linea: costo live
        char li[64];
        snprintf(li,sizeof li,"LINEA %.1f M - %d$%s",(double)gLineLen,gLineCost,
                 gLineValid?"":(gHost.plc->reason==PL_NOFUNDS?" - BUDGET!":" - OCCUPATO"));
        ui_text(gPrepCardR[0].x+2*(PREP_CARD_W+8),gPrepCardR[0].y+8,2,li,
                gLineValid?0.30f:0.95f,gLineValid?0.95f:0.25f,0.25f,1);
        ui_text(mpx+16,mpy-20,2,li,gLineValid?0.30f:0.95f,gLineValid?0.95f:0.25f,0.25f,1);
    } else if(sel){                                 // pannello info (§3)
        char l1[80],l2[96];
        prep_info_lines(sel,l1,sizeof l1,l2,sizeof l2);
        float ix=gPrepCardR[0].x+2*(PREP_CARD_W+8)+12;
        ui_text(ix,gPrepCardR[0].y+8,2,l1,0.95f,0.85f,0.45f,1);
        ui_text(ix,gPrepCardR[0].y+26,2,l2,0.80f,0.80f,0.80f,1);
    }
    { const UiRect *r=&gPrepGoR;                    // VIA ALL'ORDA (=INVIO)
      ui_quad(r->x,r->y,r->w,r->h,0.50f,0.10f,0.07f,0.95f);
      ui_text_c(r->x+r->w*0.5f,r->y+6,3,"VIA ALL'ORDA",1,0.92f,0.85f,1);
      ui_text_c(r->x+r->w*0.5f,y0+100,1.5f,"INVIO",0.6f,0.6f,0.6f,1); }
}

// ---- barra + aiming del colpo di mortaio (stato pool in cima, presso gScorch) --
static UiRect gMortR; static float gStrikeBarY = 1e9f;
static void strikes_ui_layout(int SW, int SH) {
    (void)SW; float y0 = (float)SH - PREP_BAR_H; gStrikeBarY = y0;
    gMortR = (UiRect){ 10.0f, y0 + 36, PREP_CARD_W, PREP_CARD_H };
}
static void strikes_bar_draw(int SW, int SH) {
    strikes_ui_layout(SW, SH);
    float y0 = gStrikeBarY;
    ui_quad(0, y0, (float)SW, PREP_BAR_H, 0.05f, 0.05f, 0.09f, 0.88f);
    ui_quad(0, y0, (float)SW, 2, 0.35f, 0.10f, 0.08f, 1);
    const UiRect *r = &gMortR; int on = gAimMort;
    ui_quad(r->x, r->y, r->w, r->h, on ? 0.28f : 0.12f, on ? 0.14f : 0.10f, 0.10f, 0.92f);
    ui_quad(r->x, r->y, r->w, 2, 0.65f, 0.32f, 0.12f, 1);
    float cx = r->x + r->w * 0.5f, cy = r->y + 24;
    ui_quad(cx - 10, cy - 2, 20, 7, 0.35f, 0.37f, 0.35f, 1);        // piastra
    ui_quad(cx - 2, cy - 13, 5, 12, 0.52f, 0.52f, 0.58f, 1);        // tubo
    ui_text_c(cx, r->y + r->h - 12, 1.5f, "MORTAIO", 0.95f, 0.9f, 0.85f, 1);
    ui_text(r->x + r->w + 18, y0 + 52, 2,
            on ? "MIRA COL MOUSE - CLICK SINISTRO = FUOCO (RMB/M ANNULLA)"
               : "M O CLICCA L'ICONA PER IL COLPO DI MORTAIO",
            0.85f, 0.85f, 0.85f, 1);
}
static int strikes_ui_click(float mx, float my, int SW, int SH) {
    strikes_ui_layout(SW, SH);
    if (my < gStrikeBarY) return 0;
    if (ui_hit(&gMortR, mx, my)) { gAimMort = !gAimMort; au_play(SND_MENU_SELECT); }
    return 1;                                       // sfondo barra: consumato (mai mondo)
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
        else { float left=L?L->survive_s-gSurviveT:0.0f; if(left<0)left=0;
            snprintf(line,sizeof line,"ASSALTO - RESISTI ANCORA %d S | KILLS %d%s",
                     (int)(left+0.5f),def_kills(g),
                     (gShellCore>=0&&def_struct_hp(g,gShellCore)<def_struct_hp_max(g,gShellCore))
                     ?" | IL NUCLEO E' SOTTO ATTACCO":""); }
        ui_quad(0,0,W,28,0,0,0,0.55f);
        ui_text(10,8,2,line,1,1,1,1);
        if(st==APP_PREP) prep_bar_draw(SW,SH,g,mpx,mpy);   // barra PREP (§3)
        else if(st==APP_ASSAULT) strikes_bar_draw(SW,SH);  // barra strike (mortaio)
        return;
    }
    ui_quad(0,0,W,H,0.02f,0.02f,0.04f,0.72f);           // oscura il mondo fermo
    if(st==APP_TITLE){
        ui_text_c(W*0.5f,H*0.26f,12,"HORDE",0.85f,0.15f,0.10f,1);
        ui_text_c(W*0.5f,H*0.26f+12*(FONT8_H+3),3,"SIGNORE, ABBIAMO UN PROBLEMA DI ZOMBIE",
                  0.75f,0.75f,0.75f,1);
        ui_text_c(W*0.5f,H*0.74f,3,"PREMI UN TASTO",1,1,1,0.9f);
    } else if(st==APP_MENU){
        ui_text_c(W*0.5f,H*0.12f,8,"HORDE",0.85f,0.15f,0.10f,1);
        if(gApp.campaign_done)
            ui_text_c(W*0.5f,H*0.27f,3,"CAMPAGNA COMPLETATA!",0.3f,0.9f,0.3f,1);
        float s=4,lh=s*(FONT8_H+5),y=H*0.38f;
        for(int i=0;i<APP_MENU_COUNT;i++){
            const char *lab=app_menu_label(i);
            int en=app_menu_enabled(&gApp,i),sel=(i==gApp.menu_idx);
            float w=ui_text_w(s,lab);
            if(sel) ui_quad(W*0.5f-w*0.5f-18,y-8,w+36,s*FONT8_H+16,0.45f,0.08f,0.06f,0.85f);
            float c=en?1.0f:0.4f;
            ui_text_c(W*0.5f,y,s,lab,c,c,c,1);
            y+=lh;
        }
        ui_text_c(W*0.5f,y+lh*0.5f,2,"FRECCE = SCEGLI   INVIO = CONFERMA",0.6f,0.6f,0.6f,1);
    } else if(st==APP_SETTINGS){
        ui_text_c(W*0.5f,H*0.16f,6,"SETTINGS",0.9f,0.9f,0.9f,1);
        float s=3.5f,lh=s*(FONT8_H+6),y=H*0.38f;
        for(int i=0;i<APP_SET_COUNT;i++){
            char row[64];
            if(i==APP_SET_SFX)        snprintf(row,sizeof row,"VOLUME SFX     < %2d >",gApp.vol_sfx);
            else if(i==APP_SET_MUSIC) snprintf(row,sizeof row,"VOLUME MUSICA  < %2d >",gApp.vol_music);
            else                      snprintf(row,sizeof row,"INDIETRO");
            int sel=(i==gApp.set_idx);
            float w=ui_text_w(s,row);
            if(sel) ui_quad(W*0.5f-w*0.5f-14,y-7,w+28,s*FONT8_H+14,0.45f,0.08f,0.06f,0.85f);
            ui_text_c(W*0.5f,y,s,row,1,1,1,1);
            y+=lh;
        }
        if(!au_backend_live())
            ui_text_c(W*0.5f,y+lh*0.5f,2,"AUDIO MUTO: MANCA VAT/MINIAUDIO.H (VEDI GAME_APP_DESIGN.MD)",
                      0.7f,0.6f,0.3f,1);
    } else if(st==APP_BRIEFING){
        char hdr[96];
        snprintf(hdr,sizeof hdr,"MISSIONE %d: %s",gApp.cur+1,(L&&L->name[0])?L->name:"(SENZA NOME)");
        ui_text(W*0.14f,H*0.16f,4,hdr,0.95f,0.78f,0.30f,1);
        ui_text(W*0.14f,H*0.28f,2.5f,L?L->brief:"",0.92f,0.92f,0.92f,1);
        char obj[80];
        snprintf(obj,sizeof obj,"OBIETTIVO: RESISTI %d SECONDI%s",
                 (int)(L?L->survive_s:0),(L&&L->core_hp>0)?" - DIFENDI IL NUCLEO":"");
        ui_text(W*0.14f,H*0.60f,3,obj,0.75f,0.88f,1.0f,1);
        ui_text_c(W*0.5f,H*0.80f,3,
                  gApp.level_ready?"INVIO PER INIZIARE":"CARICAMENTO...",1,1,1,0.9f);
    } else if(st==APP_DEBRIEF){
        const char *T=gApp.won?"MISSIONE COMPIUTA":"POSTAZIONE PERSA";
        ui_text_c(W*0.5f,H*0.32f,6,T,gApp.won?0.30f:0.90f,gApp.won?0.90f:0.20f,0.20f,1);
        char k[64]; snprintf(k,sizeof k,"KILLS %d",def_kills(g));
        ui_text_c(W*0.5f,H*0.50f,3,k,0.9f,0.9f,0.9f,1);
        const char *P=gApp.won?((gApp.cur+1>=gApp.nlevels)?"INVIO PER IL MENU"
                                                          :"INVIO: PROSSIMO LIVELLO")
                              :"INVIO: RIPROVA";
        ui_text_c(W*0.5f,H*0.66f,3,P,1,1,1,0.9f);
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

    // --- sagome-cadavere (richiesta utente / CORPSE_DESIGN §10.2): quad orientati
    // texturizzati con l'atlante di sprite top-down bakato dai modelli VAT in posa
    // di morte (bake init-time più sotto). Persistenti come i decal di sangue.
#define CORPSEDECMAX 4096
#define CORPSE_CELL  256                  // px per cella d'atlante
#define CORPSE_HALF  1.15f                // semi-lato ortho del bake (m): un corpo disteso
    static float cdecraw[CORPSEDECMAX*6];  // x,y,hd,size,colonna,outfit (da vat_layer)
    static float cdecinst[CORPSEDECMAX*7]; // cx,gy,cz,hd, half,colonna,outfit (istanza GL)
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
    printf("torrette 3D: light=%s heavy=%s (scala %.2f)\n",
           gTurM[0].ok?"ok":"pilastrino", gTurM[1].ok?"ok":"pilastrino", (double)gTurScale);
    int turCap=def_turret_count(g); if(turCap<NT) turCap=NT;   // >= tracer's NT cap
    // verts/torretta: pilastrino=30, oppure la parte più grossa di un modello glb
    int turVpe=30;
    for(int m=0;m<2;m++) if(gTurM[m].ok){ int v=gTurM[m].base.nv+gTurM[m].gun.nv; if(v>turVpe)turVpe=v; }
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

    // --- BAKE atlante sagome-cadavere (init-time): ortho TOP-DOWN del modello VAT
    // nell'ultimo frame della posa di morte. ALBEDO = griglia (colonna=var*posa ×
    // riga=outfit), NORMAL = una sola riga (outfit-indipendente) per il relight
    // per-pixel. Colonna = v*NPOSE+p; posa 0='dying', 1='death' (devono combaciare
    // con vat_layer_die, le due falle a terra differiscono di ~90°).
    GLuint bakeProg=vg_shader("assets/shaders/vat.vs","assets/shaders/corpsebake.fs");
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
                        float one[14]={0,0,0, 0, 1.0f, (float)fr,(float)fr,0, (float)(o+16), 0.55f,0.5f,0.5f, 0,0};
                        glViewport(col*CORPSE_CELL,o*CORPSE_CELL,CORPSE_CELL,CORPSE_CELL);
                        glBindBuffer(GL_ARRAY_BUFFER,bi);glBufferSubData(GL_ARRAY_BUFFER,0,14*sizeof(float),one);
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

    // --- gore mesh-gibs (FX_LAB): VAO delle mesh di assets/models/gibs.glb + shader mesh
    // statica texturizzata (diffuse di zombie_man, A[0]). Pool fisico in vat_layer.
    GibMesh GM[8]={0}; int nGibMesh=load_gib_meshes("assets/models/gibs.glb",GM,8);
    GLuint mProg=vg_shader("assets/shaders/mesh.vs","assets/shaders/mesh.fs");
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
    gHost.ground_on=groundOn; gHost.cam_x=&cx; gHost.cam_z=&cz; gHost.running=&running;
    gHost.plc=&plc; gHost.traps=&traps; prep_tabs_build(&plc);   // barra PREP: indice tab->voci (§7)
    gUiBuf=malloc((size_t)UI_MAX_V*9*sizeof(float));
    GLuint uiProg=vg_shader("assets/shaders/flat.vs","assets/shaders/ui.fs");
    GLint uVPui=glGetUniformLocation(uiProg,"uVP");
    GLuint uiVao,uiVbo; glGenVertexArrays(1,&uiVao);glBindVertexArray(uiVao);
    glGenBuffers(1,&uiVbo);glBindBuffer(GL_ARRAY_BUFFER,uiVbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)UI_MAX_V*9*sizeof(float),NULL,GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,9*sizeof(float),(void*)0);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,3,GL_FLOAT,0,9*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
    glVertexAttribPointer(2,3,GL_FLOAT,0,9*sizeof(float),(void*)(6*sizeof(float)));glEnableVertexAttribArray(2);
    glBindVertexArray(0);
#endif
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
#ifdef GAME_SHELL
                // negli stati UI (menu/briefing/…) il mouse non tocca il mondo
                if(!ed.active && gApp.state!=APP_PREP && gApp.state!=APP_ASSAULT)
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
                        continue;
                    }
                    if(gAimMort){
                        float wx,wy;
                        if(pick_y0(vp,mxf,myf,SW,SH,&wx,&wy)){
                            gAimX=wx; gAimY=wy;
                            if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN){
                                if(e.button.button==SDL_BUTTON_LEFT){
                                    strike_add(wx,wy,gBaseOX,gBaseOY,MORTAR_DELAY);
                                    // puff di lancio dal container: lampo + fumo alla bocca
                                    float lo[3]={gBaseOX,ter_z(gBaseOX,gBaseOY)+BASE_H,gBaseOY};
                                    fx_emit(&fx,lo,&MUZZLE_FLASH_HVY_DEF,0.0f,-1.0f);
                                    fx_emit(&fx,lo,&EXPL_SMOKE_DEF,0.0f,-1.0f);
                                    au_play(SND_MENU_SELECT); }
                                else if(e.button.button==SDL_BUTTON_RIGHT) gAimMort=0;
                            }
                        }
                        continue;                  // in aiming il mouse non guida la camera
                    }
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
                        plc.active=0;
#ifdef GAME_SHELL
                        plineOn=0; plineChain=0;   // esistono solo nella shell
#endif
                        continue; }
                    float wx,wy; if(pick_y0(vp,mxf,myf,SW,SH,&wx,&wy)) pl_set_cursor(&plc,wx,wy);
#ifdef GAME_SHELL
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
                        SDL_SetWindowFullscreen(win,fullscreen); continue; }
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
                    shell_do_act(app_input(&gApp,
                        e.key.key==SDLK_ESCAPE?APP_IN_BACK:APP_IN_CONFIRM));
                    continue;
                }
            }
#endif
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
                case SDLK_M:  // colpo di mortaio: entra/esci dall'aiming (solo in ASSALTO)
                    if(gApp.state==APP_ASSAULT){ gAimMort=!gAimMort;
                        if(gAimMort) au_play(SND_MENU_SELECT); }
                    break;
#endif
                case SDLK_RETURN: case SDLK_KP_ENTER:  // fase A: via all'assalto
                    if(gMissionOn && gMission.state==MISSION_PREP){
                        mission_go(&gMission); plc.active=0;
                        printf("missione: ASSALTO\n"); }
                    break;
                case SDLK_P:    // piazzamento runtime: attiva/disattiva (budget di prova se 0)
                    if(gMissionOn && !mission_placement_open(&gMission)){
                        printf("placement: solo in PREP (fase A)\n"); break; }
                    plc.active=!plc.active;
                    if(plc.active && def_budget(g)<=0){ def_set_budget(g,99999); printf("placement ON (budget di prova)\n"); }
                    break;
#ifdef GAME_SHELL
                case SDLK_LEFTBRACKET:  if(plc.active) prep_cycle(-1); break;
                case SDLK_RIGHTBRACKET: if(plc.active) prep_cycle(+1); break;
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
        int sim_run = !ed.active && !paused;
#ifdef GAME_SHELL
        sim_run = sim_run && (gApp.state==APP_PREP || gApp.state==APP_ASSAULT);
#endif
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
                Uint64 t0=SDL_GetPerformanceCounter();
                simp_step(s,FIXED_DT);
                Uint64 t1=SDL_GetPerformanceCounter();
                def_update(g,FIXED_DT);
                // mine (GAME_PLAN fase D): dopo lo step (griglia fresca) le trappole
                // ARMATE scattano sul primo agente vicino -> host_blast (def_blast +
                // FX, friendly fire incluso). Catene risolte in ordine di id nel core.
                { TrapBlastCtx tbc={g,s,&sc,&gCatalog,&dz,&fx,vl,0};
                  traps_update(&traps,s,FIXED_DT,on_trap_blast,&tbc);
                  if(tbc.prop_changed) prNV=upload_prop_mesh(prVbo,&sc,&gCatalog,&dz,g); }
#ifdef GAME_SHELL
                // colpi di mortaio in volo (BASE_DESIGN §3): il proiettile parte dal
                // container (ox,oy) e segue un arco parabolico fino al bersaglio (x,y),
                // che raggiunge esattamente a t<=0 -> host_blast (impatto). Il proiettile
                // è FICTION: una testata billboard luminosa + scia di fumo emessa lungo
                // il percorso (fx_emit_one, niente stato persistente). Precise, no RNG.
                for(int i=0;i<STRIKE_MAX;i++) if(gStrikes[i].on){
                    gStrikes[i].t-=FIXED_DT;
                    if(gStrikes[i].t<=0.0f){ gStrikes[i].on=0;
                        if(host_blast(g,s,&sc,&gCatalog,&dz,&fx,vl,gStrikes[i].x,gStrikes[i].y,
                                      BLAST_R,BLAST_DMG,blast_str,blast_up))
                            prNV=upload_prop_mesh(prVbo,&sc,&gCatalog,&dz,g);
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
                { int nt=def_turret_count(g);
                  for(int ti=0;ti<nt;ti++){ DefTurret *t=def_turret(g,ti);
                      if(!t->fired || def_turret_disabled(g,ti)) continue;
                      anim_fire(&gAnim,ANIM_TURRET_RECOIL,ti,0.12f);
                      float ca=cosf(t->ang), sa=sinf(t->ang);
                      TurretModel *tm=&gTurM[t->heavy?1:0];
                      float mh=tm->ok?tm->muzzle_h:0.9f, mx=tm->ok?tm->muzzle_x:0.0f;
                      float ox=t->x+ca*mx, oz=t->y+sa*mx, oy=mh+ter_z(t->x,t->y);
                      float ex=t->x+ca*t->last_t, ez=t->y+sa*t->last_t, ey=mh+ter_z(ex,ez);
                      tracer_spawn(ox,oy,oz, ex,ey,ez, t->heavy);
                      float mo[3]={ox,oy,oz};
                      fx_emit(&fx,mo, t->heavy?&MUZZLE_FLASH_HVY_DEF:&MUZZLE_FLASH_DEF, t->ang, 0.35f);
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
                pl_validate(&plc,g,s);                      // colore fresco ogni frame
                float r = it ? fmaxf(0.5f, 0.5f*fmaxf(it->w,fmaxf(it->h,it->radius*2.0f))) : 0.6f;
                float cr = plc.valid?0.20f:0.95f, cg = plc.valid?0.90f:0.18f, cb=0.18f;
                ov = ed_push_marker(edovl,0,plc.cx,plc.cy,r, cr,cg,cb);
#ifdef GAME_SHELL
                if(it && it->kind==PL_TURRET){              // cerchio di gittata (§8)
                    float R=it->range>0.0f?it->range:40.0f;
                    const int NSEG=64;
                    for(int k=0;k<NSEG && ov+6<=EDOVL_MAXV;k++){
                        float a0=(float)k*6.2831853f/NSEG, a1=(float)(k+1)*6.2831853f/NSEG;
                        ov=ed_push_bar(edovl,ov,plc.cx+cosf(a0)*R,plc.cy+sinf(a0)*R,
                                       plc.cx+cosf(a1)*R,plc.cy+sinf(a1)*R,0.3f,cr,cg,cb);
                    }
                }
#endif
            }
            if(ov){ glDisable(GL_DEPTH_TEST);
                glUseProgram(progFlat);glUniformMatrix4fv(uVPflat,1,GL_FALSE,vp);
                glBindVertexArray(ovVao);glBindBuffer(GL_ARRAY_BUFFER,ovVbo);
                glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)ov*9*sizeof(float),edovl);
                glDrawArrays(GL_TRIANGLES,0,ov);
                glEnable(GL_DEPTH_TEST); }
        }
#ifdef GAME_SHELL
        // X di mira del mortaio: due barre incrociate sul punto mirato (aiming
        // attivo in ASSALTO). Riusa l'overlay flat del ghost di placement.
        if(gAimMort && gApp.state==APP_ASSAULT && !ed.active){
            int ov=0; float sX=1.4f;
            ov=ed_push_bar(edovl,ov, gAimX-sX,gAimY-sX, gAimX+sX,gAimY+sX, 0.35f, 0.95f,0.25f,0.12f);
            ov=ed_push_bar(edovl,ov, gAimX-sX,gAimY+sX, gAimX+sX,gAimY-sX, 0.35f, 0.95f,0.25f,0.12f);
            if(ov){ glDisable(GL_DEPTH_TEST);
                glUseProgram(progFlat);glUniformMatrix4fv(uVPflat,1,GL_FALSE,vp);
                glBindVertexArray(ovVao);glBindBuffer(GL_ARRAY_BUFFER,ovVbo);
                glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)ov*9*sizeof(float),edovl);
                glDrawArrays(GL_TRIANGLES,0,ov);
                glEnable(GL_DEPTH_TEST); }
        }
        // overlay shell (title/menu/briefing/debrief + barra di fase): 2D in
        // pixel, sopra tutto, blend alpha (l'alpha viaggia in aNormal.x).
        gUiV=0; shell_build_ui(SW,SH,g,mouse_px,mouse_py);
        if(gUiV>0){
            glDisable(GL_DEPTH_TEST); glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
            mat4 uivp; m_ortho(uivp,0.0f,(float)SW,(float)SH,0.0f,-1.0f,1.0f);
            glUseProgram(uiProg); glUniformMatrix4fv(uVPui,1,GL_FALSE,uivp);
            glBindVertexArray(uiVao); glBindBuffer(GL_ARRAY_BUFFER,uiVbo);
            glBufferSubData(GL_ARRAY_BUFFER,0,(GLsizeiptr)gUiV*9*sizeof(float),gUiBuf);
            glDrawArrays(GL_TRIANGLES,0,gUiV);
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
            char wv[80]="";
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
                snprintf(wv,sizeof wv," | %s%s%s budget %d",mst,tls,pls,def_budget(g)); }
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
