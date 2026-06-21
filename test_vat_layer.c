/* test_vat_layer.c — headless verification of the VAT render-layer animation
 * bookkeeping (M6): the HIT one-shot overlay and the DEATH decedent pool. No GL
 * (vat_layer.c only needs sim_particles.h); a synthetic meta decouples it from
 * the baked assets. ATTACK (wall_pressure-driven) is verified visually.
 *
 * Clip layout of the synthetic meta (frames are global VAT rows):
 *   walk  [ 0..10)  idle [10..20)  hit [20..30)  death [30..40)
 */
#include "vat/vat_layer.h"
#include "sim_particles.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define META "test_vat_layer_meta.txt"

static int in_range(float f, int lo, int hi){ return f>=lo && f<hi; }

int main(void){
    FILE *f=fopen(META,"w");
    if(!f){ printf("FAIL (tmp)\n"); return 1; }
    fputs("texW=64\ntexH=64\nrowsPerFrame=1\nfps=30\nscale=1\ntotalFrames=40\n"
          "clip=walk startFrame=0 numFrames=10 duration_s=1 stride_m=1\n"
          "clip=idle startFrame=10 numFrames=10 duration_s=1 stride_m=0\n"
          "clip=hit startFrame=20 numFrames=10 duration_s=0.5 stride_m=0\n"
          "clip=death startFrame=30 numFrames=10 duration_s=1 stride_m=0\n", f);
    fclose(f);

    int ok=1;
    SimP *s=simp_create(40,40,0.5f,64);
    int idx=simp_spawn(s,5,5);
    if(idx<0){ printf("FAIL (spawn)\n"); return 1; }

    const char *paths[1]={META};
    VatLayer *vl=vat_layer_create_multi(paths,1,64);

    const float dt=1.0f/60.0f;
    float buf[64*12];

    vat_layer_update(vl,s,dt);                  /* slot seen, state initialised */
    int slot=simp_slot_of(s,idx);
    int c=vat_layer_fill_variant(vl,s,0,buf,64);
    ok = ok && c==1 && in_range(buf[5],0,20);   /* live, locomotion/idle (not hit/death) */
    printf("baseline: count=%d gA=%.0f | %s\n", c, (double)buf[5], ok?"ok":"BAD");

    /* --- HIT one-shot: overrides with the hit clip, then resumes --- */
    vat_layer_hit(vl,slot);
    vat_layer_update(vl,s,dt);
    c=vat_layer_fill_variant(vl,s,0,buf,64);
    int hit_ok = c==1 && in_range(buf[5],20,30);
    printf("hit active: gA=%.0f | %s\n", (double)buf[5], hit_ok?"ok":"BAD");
    ok = ok && hit_ok;

    for(int k=0;k<40;k++) vat_layer_update(vl,s,dt);   /* > 0.5 s: flinch ends */
    c=vat_layer_fill_variant(vl,s,0,buf,64);
    int resume_ok = c==1 && in_range(buf[5],0,20);
    printf("hit resumed: gA=%.0f | %s\n", (double)buf[5], resume_ok?"ok":"BAD");
    ok = ok && resume_ok;

    /* --- DEATH decedent: persists after the sim agent is gone --- */
    vat_layer_die(vl,slot,5,5,0.30f);
    simp_kill(s,idx);                            /* agent removed from the sim */
    vat_layer_update(vl,s,dt);
    c=vat_layer_fill_variant(vl,s,0,buf,64);
    int dead_ok = simp_count(s)==0 && c==1 && in_range(buf[5],30,40);  /* decedent only, death clip */
    printf("decedent: live=%d count=%d gA=%.0f | %s\n",
           simp_count(s), c, (double)buf[5], dead_ok?"ok":"BAD");
    ok = ok && dead_ok;

    /* hold the last frame, then free after TTL (death dur 1 s + 2 s hold) */
    c=vat_layer_fill_variant(vl,s,0,buf,64);
    float held=buf[5];
    for(int k=0;k<70;k++) vat_layer_update(vl,s,dt);   /* ~1.2 s: clip done, still held */
    c=vat_layer_fill_variant(vl,s,0,buf,64);
    int hold_ok = c==1 && buf[5]>=held && in_range(buf[5],30,40);
    printf("hold last frame: gA=%.0f (was %.0f) | %s\n",(double)buf[5],(double)held,hold_ok?"ok":"BAD");
    ok = ok && hold_ok;

    for(int k=0;k<140;k++) vat_layer_update(vl,s,dt);  /* > TTL total: freed */
    c=vat_layer_fill_variant(vl,s,0,buf,64);
    int gone_ok = c==0;
    printf("decedent freed: count=%d | %s\n", c, gone_ok?"ok":"BAD");
    ok = ok && gone_ok;

    vat_layer_destroy(vl); simp_destroy(s); remove(META);
    printf(ok?"PASS\n":"FAIL\n");
    return ok?0:1;
}
