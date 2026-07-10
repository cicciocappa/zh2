/* test_vat_layer.c — headless verification of the VAT render-layer animation
 * bookkeeping (M6): the HIT one-shot overlay, the external ATTACK latch
 * (vat_layer_attack — the wall_pressure-driven path is verified visually) and
 * the DEATH decedent pool. No GL (vat_layer.c only needs sim_particles.h); a
 * synthetic meta decouples it from the baked assets.
 *
 * Clip layout of the synthetic meta (frames are global VAT rows):
 *   walk [0..10)  idle [10..20)  hit [20..30)  death [30..40)  attack [40..50)
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
    fputs("texW=64\ntexH=64\nrowsPerFrame=1\nfps=30\nscale=1\ntotalFrames=50\n"
          "clip=walk startFrame=0 numFrames=10 duration_s=1 stride_m=1\n"
          "clip=idle startFrame=10 numFrames=10 duration_s=1 stride_m=0\n"
          "clip=hit startFrame=20 numFrames=10 duration_s=0.5 stride_m=0\n"
          "clip=death startFrame=30 numFrames=10 duration_s=1 stride_m=0\n"
          "clip=attack startFrame=40 numFrames=10 duration_s=1 stride_m=0\n", f);
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

    /* --- HIT one-shot (HARD CUT): overrides with the hit clip, then resumes --- */
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

    /* --- ATTACK latch (external, vat_layer_attack): the attack clip plays
     * while the host re-arms it each frame, then decays back (ATTACK_HOLD
     * 1 s) to locomotion once the host stops calling. --- */
    for(int k=0;k<30;k++){                       /* > BLEND_DUR: fade completes */
        vat_layer_attack(vl,slot,1.0f,0.0f);
        vat_layer_update(vl,s,dt);
    }
    c=vat_layer_fill_variant(vl,s,0,buf,64);
    int atk_ok = c==1 && in_range(buf[5],40,50);
    printf("attack latched: gA=%.0f | %s\n", (double)buf[5], atk_ok?"ok":"BAD");
    ok = ok && atk_ok;

    for(int k=0;k<90;k++) vat_layer_update(vl,s,dt);   /* > hold+blend: released */
    c=vat_layer_fill_variant(vl,s,0,buf,64);
    int rel_ok = c==1 && in_range(buf[5],0,20);
    printf("attack released: gA=%.0f | %s\n", (double)buf[5], rel_ok?"ok":"BAD");
    ok = ok && rel_ok;

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

    /* --- bug2 regression: swapping body mid-flinch must NOT render a stale
     * clip from the OLD layout. A HIT sets osClip = variant0's hit-clip index;
     * switching to a SHORTER-layout body (the crawler has no hit clip) used to
     * leave osClip pointing past the new body's clip array -> out-of-bounds
     * frame = distorted/frozen pose for the flinch duration. set_variant now
     * cancels the flinch, so we must see the new body's walk frames at once. */
    {
        const char *MA="test_vat_v0_meta.txt", *MB="test_vat_v1_meta.txt";
        FILE *fa=fopen(MA,"w"); fputs("texW=64\ntexH=64\nrowsPerFrame=1\nfps=30\nscale=1\ntotalFrames=30\n"
            "clip=walk startFrame=0 numFrames=10 duration_s=1 stride_m=1\n"
            "clip=hit startFrame=20 numFrames=10 duration_s=0.5 stride_m=0\n", fa); fclose(fa);
        /* variant1 = crawler-like: a SINGLE walk clip at a high startFrame, no hit */
        FILE *fb=fopen(MB,"w"); fputs("texW=64\ntexH=64\nrowsPerFrame=1\nfps=30\nscale=1\ntotalFrames=106\n"
            "clip=walk startFrame=100 numFrames=6 duration_s=1 stride_m=1\n", fb); fclose(fb);

        SimP *s2=simp_create(40,40,0.5f,64);
        int i2=simp_spawn(s2,5,5);
        const char *paths2[2]={MA,MB};
        VatLayer *vl2=vat_layer_create_multi(paths2,2,64);
        int sl=simp_slot_of(s2,i2);
        vat_layer_pin_variant(vl2,sl,0);            /* nasce body 0 (ha la hit) */
        vat_layer_update(vl2,s2,dt);
        vat_layer_hit(vl2,sl);                      /* flinch in corso: osClip = hit di v0 */
        vat_layer_update(vl2,s2,dt);
        int c2=vat_layer_fill_variant(vl2,s2,0,buf,64);
        int flinching = c2==1 && in_range(buf[5],20,30);
        vat_layer_set_variant(vl2,sl,1);            /* swap a body corto mid-flinch */
        vat_layer_update(vl2,s2,dt);
        c2=vat_layer_fill_variant(vl2,s2,1,buf,64); /* ora rende come variante 1 */
        int swap_ok = c2==1 && in_range(buf[5],100,106);  /* walk di v1, NON frame stantio/OOB */
        printf("mid-flinch swap: flinching=%d gA=%.0f | %s\n",
               flinching, (double)buf[5], (flinching&&swap_ok)?"ok":"BAD");
        ok = ok && flinching && swap_ok;
        vat_layer_destroy(vl2); simp_destroy(s2); remove(MA); remove(MB);
    }

    /* --- hit window: only a random [1..30]+[20..30]-frame slice of the hit clip
     * plays (shorter flinch than the full ~61-frame clip + variety). The flinch
     * duration must land in [20,30]/fps and the rendered frame must stay inside
     * the clip's window region; successive hits must vary. --- */
    {
        const char *MW="test_vat_hw_meta.txt";
        FILE *fw=fopen(MW,"w"); fputs("texW=64\ntexH=64\nrowsPerFrame=1\nfps=30\nscale=1\ntotalFrames=200\n"
            "clip=walk startFrame=0 numFrames=10 duration_s=1 stride_m=1\n"
            "clip=hit startFrame=100 numFrames=61 duration_s=2.0333 stride_m=0\n", fw); fclose(fw);
        const char *pw[1]={MW};
        SimP *s3=simp_create(40,40,0.5f,64);
        VatLayer *vl3=vat_layer_create_multi(pw,1,64);
        int dur_ok=1, frame_ok=1, varied=0; float prev=-1, dmin=9, dmax=0;
        for(int k=0;k<8;k++){
            int i3=simp_spawn(s3,5,5); vat_layer_update(vl3,s3,dt);
            int sl=simp_slot_of(s3,i3);
            float fdur=vat_layer_hit(vl3,sl);          /* slice duration (s) */
            if(fdur<20.0f/30.0f-1e-3f || fdur>30.0f/30.0f+1e-3f) dur_ok=0;
            if(fdur<dmin)dmin=fdur; if(fdur>dmax)dmax=fdur;
            if(prev>=0 && fdur!=prev) varied=1; prev=fdur;
            vat_layer_update(vl3,s3,dt);
            int c3=vat_layer_fill_variant(vl3,s3,0,buf,64);
            if(!(c3==1 && buf[5]>=100 && buf[5]<161)) frame_ok=0;   /* inside hit clip [100,161) */
            simp_kill(s3,i3); vat_layer_update(vl3,s3,dt);          /* free slot for reuse */
        }
        printf("hit window: dur=[%.3f,%.3f]s frame-in-window=%d varied=%d | %s\n",
               (double)dmin,(double)dmax,frame_ok,varied,(dur_ok&&frame_ok&&varied)?"ok":"BAD");
        ok = ok && dur_ok && frame_ok && varied;
        vat_layer_destroy(vl3); simp_destroy(s3); remove(MW);
    }

    printf(ok?"PASS\n":"FAIL\n");
    return ok?0:1;
}
