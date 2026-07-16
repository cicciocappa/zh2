/* balance.c — implementation. See balance.h. */

#include "balance.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <ctype.h>

void balance_defaults(Balance *b) {
    memset(b, 0, sizeof *b);
    def_tuning_defaults(&b->def);

    /* turrets per kind: the "torrette 2.0" placement-catalog numbers
     * (PL_CAT_GAME + place.c per-kind defaults + BIOMASS §5 mags/costs).
     * Scene turrets now read the same table (they used to carry their own
     * light 0.10s/55HP — unified 2026-07-15, retune here if missed). */
    b->tur[TUR_LIGHT] = (BalTurret){ 40.0f, 0.12f, 40.0f, 90.0f, 250.0f, 100, 40, 25.0f };
    b->tur[TUR_HEAVY] = (BalTurret){ 40.0f, 0.50f,  0.0f, 90.0f, 250.0f, 250, 12, 35.0f };
    b->tur[TUR_FLAME] = (BalTurret){ 12.0f, 0.15f,  6.0f, 90.0f, 250.0f, 180, 30, 30.0f };
    b->tur[TUR_ACID]  = (BalTurret){ 18.0f, 0.25f,  4.0f, 90.0f, 250.0f, 220, 25, 30.0f };
    b->turret_reload_s = 12.0f;      /* §5 v2: the silent window must hurt   */

    b->barricade.cost = 12;  b->barricade.hp = 300.0f; b->barricade.mass = 30.0f;
    b->fence.cost     = 20;  b->fence.hp     = 200.0f; b->fence.mass     = 15.0f;
    b->fence.opacity  = 0.3f;
    b->mine.cost = 40; b->mine.trigger_r = 1.2f; b->mine.blast_r = 6.0f;
    b->mine.damage = 150.0f; b->mine.strength = 22.0f; b->mine.up_ratio = 0.6f;
    b->mine.arm_s = 1.0f;

    b->mortar.cost = 40.0f;  b->mortar.delay = 1.8f;
    b->mortar.min_range = 12.0f; b->mortar.max_range = 90.0f;
    b->mortar.cooldown = 5.0f;
    b->mortar.blast_r = 8.0f; b->mortar.blast_damage = 180.0f;
    /* LOOP_DESIGN D (2026-07-15): mortar kills yield a fraction of biomass
     * (the blast pulverizes it — kills the self-feeding farm loop) and the
     * crater stamps blood-fear (route-denial tool: 2x danger_ref stays
     * saturated for ~one half-life, ~30 s with core defaults). */
    b->mortar.bio_yield = 0.1f;
    b->mortar.fear_w = 16.0f; b->mortar.fear_r = 8.0f;

    b->bio.cap = 500.0f; b->bio.repair_rate = 100.0f; b->bio.adjust_cost = 0.0f;
    b->bio.build_markup = 1.5f;
    b->bio.yield[BT_OBESE] = 2.0f; b->bio.yield[BT_MAN]   = 1.0f;
    b->bio.yield[BT_WOMAN] = 1.0f; b->bio.yield[BT_CHILD] = 1.0f;
    b->bio.yield[BT_TANK]  = 8.0f;

    b->soldier.hp = 120.0f; b->soldier.speed = 4.2f; b->soldier.accel = 30.0f;
    b->soldier.radius = 0.35f; b->soldier.mass = 15.0f;
    b->soldier.touch_dps = 15.0f; b->soldier.touch_knock = 20.0f;
    b->soldier.gun_range = 16.0f; b->soldier.gun_period = 0.10f;
    b->soldier.gun_damage = 35.0f; b->soldier.down_s = 8.0f;
    b->soldier.lure_w = -0.8f; b->soldier.lure_r = 10.0f;
    b->soldier.grenade_cost = 30.0f; b->soldier.grenade_range = 12.0f;
    b->soldier.grenade_r = 5.0f; b->soldier.grenade_damage = 140.0f;
    b->soldier.grenade_strength = 18.0f; b->soldier.grenade_up = 0.5f;
    b->soldier.bio_yield = 1.0f;

    b->lure.weight = -0.5f; b->lure.radius = 6.0f; b->lure.linger = 2.5f;
    b->contact.turret_dps = 12.0f; b->contact.turret_reach = 2.0f;
    b->director.base_rate = 16.0f; b->director.rate_ramp = 8.0f;
    b->director.wave_period = 15.0f;
    b->wave.pause = 15.0f; b->wave.bonus_per_s = 2.0f;

    b->budget = 1000;
    b->lz_hp = 1500.0f;
    b->fall_damage = 25.0f;
}

/* ---- key table --------------------------------------------------------- */

typedef struct { const char *key; char type; size_t off; } BalKey;
#define KF(name, field) { name, 'f', offsetof(Balance, field) }
#define KI(name, field) { name, 'i', offsetof(Balance, field) }

#define ENEMY_KEYS(tag, BT) \
    KI("enemy." tag ".hp",         def.enemy[BT].hp_max),     \
    KF("enemy." tag ".radius",     def.enemy[BT].radius),     \
    KF("enemy." tag ".speed",      def.enemy[BT].v_pref),     \
    KF("enemy." tag ".mass",       def.enemy[BT].mass),       \
    KI("enemy." tag ".heavy_hits", def.enemy[BT].heavy_hits)

#define TURRET_KEYS(tag, K) \
    KF("turret." tag ".range",       tur[K].range),           \
    KF("turret." tag ".fire_period", tur[K].fire_period),     \
    KF("turret." tag ".damage",      tur[K].damage),          \
    KF("turret." tag ".arc_deg",     tur[K].arc_deg),         \
    KF("turret." tag ".hp",          tur[K].hp),              \
    KI("turret." tag ".cost",        tur[K].cost),            \
    KI("turret." tag ".mag",         tur[K].mag),             \
    KF("turret." tag ".reload_cost", tur[K].reload_cost)

static const BalKey KEYS[] = {
    ENEMY_KEYS("obese", BT_OBESE),
    ENEMY_KEYS("man",   BT_MAN),
    ENEMY_KEYS("woman", BT_WOMAN),
    ENEMY_KEYS("child", BT_CHILD),
    ENEMY_KEYS("tank",  BT_TANK),
    KF("enemy.crawl_speed",     def.v_crawl),
    KF("gore.corpse_prob",      def.p_corpse),
    KF("gore.corpse_ttl",       def.corpse_ttl),

    TURRET_KEYS("light", TUR_LIGHT),
    TURRET_KEYS("heavy", TUR_HEAVY),
    TURRET_KEYS("flame", TUR_FLAME),
    TURRET_KEYS("acid",  TUR_ACID),
    KF("turret.flame.dot_dps",  def.burn_dps),
    KF("turret.flame.dot_dur",  def.burn_dur),
    KF("turret.acid.dot_dps",   def.acid_dps),
    KF("turret.acid.dot_dur",   def.acid_dur),
    KF("turret.reload_s",       turret_reload_s),

    KI("barricade.cost",        barricade.cost),
    KF("barricade.hp",          barricade.hp),
    KF("barricade.mass",        barricade.mass),
    KI("fence.cost",            fence.cost),
    KF("fence.hp",              fence.hp),
    KF("fence.mass",            fence.mass),
    KF("fence.opacity",         fence.opacity),
    KI("mine.cost",             mine.cost),
    KF("mine.trigger_radius",   mine.trigger_r),
    KF("mine.blast_radius",     mine.blast_r),
    KF("mine.damage",           mine.damage),
    KF("mine.strength",         mine.strength),
    KF("mine.up_ratio",         mine.up_ratio),
    KF("mine.arm_s",            mine.arm_s),

    KF("mortar.cost",           mortar.cost),
    KF("mortar.delay",          mortar.delay),
    KF("mortar.min_range",      mortar.min_range),
    KF("mortar.max_range",      mortar.max_range),
    KF("mortar.cooldown",       mortar.cooldown),
    KF("mortar.blast_radius",   mortar.blast_r),
    KF("mortar.blast_damage",   mortar.blast_damage),
    KF("mortar.bio_yield",      mortar.bio_yield),
    KF("mortar.fear",           mortar.fear_w),
    KF("mortar.fear_radius",    mortar.fear_r),

    KF("bio.cap",               bio.cap),
    KF("bio.repair_rate",       bio.repair_rate),
    KF("bio.adjust_cost",       bio.adjust_cost),
    KF("bio.build_markup",      bio.build_markup),
    KF("bio.yield.obese",       bio.yield[BT_OBESE]),
    KF("bio.yield.man",         bio.yield[BT_MAN]),
    KF("bio.yield.woman",       bio.yield[BT_WOMAN]),
    KF("bio.yield.child",       bio.yield[BT_CHILD]),
    KF("bio.yield.tank",        bio.yield[BT_TANK]),

    KF("siege.attack_period",   def.attack_period),
    KF("siege.attack_damage",   def.attack_damage),
    KF("siege.turret_dps",      contact.turret_dps),
    KF("siege.turret_reach",    contact.turret_reach),
    KF("fear.blood_radius",     def.danger_r),
    KF("fear.blood_weight",     def.danger_w),
    KF("soldier.hp",               soldier.hp),
    KF("soldier.speed",            soldier.speed),
    KF("soldier.accel",            soldier.accel),
    KF("soldier.radius",           soldier.radius),
    KF("soldier.mass",             soldier.mass),
    KF("soldier.touch_dps",        soldier.touch_dps),
    KF("soldier.knockback",        soldier.touch_knock),
    KF("soldier.gun_range",        soldier.gun_range),
    KF("soldier.gun_period",       soldier.gun_period),
    KF("soldier.gun_damage",       soldier.gun_damage),
    KF("soldier.down_s",           soldier.down_s),
    KF("soldier.lure_weight",      soldier.lure_w),
    KF("soldier.lure_radius",      soldier.lure_r),
    KF("soldier.grenade_cost",     soldier.grenade_cost),
    KF("soldier.grenade_range",    soldier.grenade_range),
    KF("soldier.grenade_radius",   soldier.grenade_r),
    KF("soldier.grenade_damage",   soldier.grenade_damage),
    KF("soldier.grenade_strength", soldier.grenade_strength),
    KF("soldier.grenade_up",       soldier.grenade_up),
    KF("soldier.bio_yield",        soldier.bio_yield),

    KF("lure.weight",           lure.weight),
    KF("lure.radius",           lure.radius),
    KF("lure.linger",           lure.linger),

    KF("director.base_rate",    director.base_rate),
    KF("director.rate_ramp",    director.rate_ramp),
    KF("director.wave_period",  director.wave_period),

    KF("wave.pause",            wave.pause),
    KF("wave.bonus",            wave.bonus_per_s),
    KI("director.tank_base",    def.mix_tank_base),
    KI("director.tank_ramp",    def.mix_tank_ramp),
    KI("director.tank_cap",     def.mix_tank_cap),
    KI("director.obese_base",   def.mix_obese_base),
    KI("director.obese_ramp",   def.mix_obese_ramp),
    KI("director.obese_cap",    def.mix_obese_cap),
    KI("director.man_pct",      def.mix_man_pct),
    KI("director.woman_pct",    def.mix_woman_pct),

    KI("game.budget",           budget),
    KF("game.lz_hp",            lz_hp),
    KF("game.fall_damage",      fall_damage),
};
#define NKEYS ((int)(sizeof(KEYS) / sizeof(KEYS[0])))

/* ---- parser ------------------------------------------------------------- */

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;
    *e = 0;
    return s;
}

int balance_load(Balance *b, const char *path, int *bad) {
    if (bad) *bad = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char line[512];
    int ln = 0, applied = 0, nbad = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *h = strchr(line, '#');           /* comment: full or trailing */
        if (h) *h = 0;
        char *eq = strchr(line, '=');
        if (!eq) {
            if (*trim(line)) {                 /* junk without '=' */
                fprintf(stderr, "balance %s:%d: riga senza '='\n", path, ln);
                nbad++;
            }
            continue;
        }
        *eq = 0;
        char *k = trim(line), *v = trim(eq + 1);
        if (!*k || !*v) {
            fprintf(stderr, "balance %s:%d: chiave o valore vuoto\n", path, ln);
            nbad++;
            continue;
        }
        const BalKey *bk = NULL;
        for (int i = 0; i < NKEYS; i++)
            if (strcmp(k, KEYS[i].key) == 0) { bk = &KEYS[i]; break; }
        if (!bk) {
            fprintf(stderr, "balance %s:%d: chiave ignota '%s'\n", path, ln, k);
            nbad++;
            continue;
        }
        char *end = NULL;
        double d = strtod(v, &end);
        if (end == v || *trim(end)) {
            fprintf(stderr, "balance %s:%d: valore non numerico '%s'\n", path, ln, v);
            nbad++;
            continue;
        }
        void *p = (char *)b + bk->off;
        if (bk->type == 'f') *(float *)p = (float)d;
        else                 *(int *)p   = (int)d;
        applied++;
    }
    fclose(f);
    if (bad) *bad = nbad;
    return applied;
}

int balance_save(const Balance *b, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    for (int i = 0; i < NKEYS; i++) {
        const void *p = (const char *)b + KEYS[i].off;
        if (KEYS[i].type == 'f')
            fprintf(f, "%s = %.9g\n", KEYS[i].key, (double)*(const float *)p);
        else
            fprintf(f, "%s = %d\n", KEYS[i].key, *(const int *)p);
    }
    fclose(f);
    return 0;
}
