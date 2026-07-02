/* anim.c — fixed pool of one-shot envelopes (see anim.h). */
#include "anim.h"

void anim_init(AnimSys *as){ as->n = 0; }

static int find_ch(const AnimSys *as, int kind, int id){
    for (int i = 0; i < as->n; i++)
        if (as->ch[i].kind == kind && as->ch[i].id == id) return i;
    return -1;
}

void anim_fire(AnimSys *as, int kind, int id, float dur){
    if (dur <= 0.0f) return;
    int i = find_ch(as, kind, id);
    if (i < 0){
        if (as->n < ANIM_MAX_CH) i = as->n++;
        else {                        /* recycle the channel closest to expiry */
            i = 0;
            float best = as->ch[0].dur - as->ch[0].t;
            for (int k = 1; k < ANIM_MAX_CH; k++){
                float left = as->ch[k].dur - as->ch[k].t;
                if (left < best){ best = left; i = k; }
            }
        }
    }
    as->ch[i].kind = kind; as->ch[i].id = id;
    as->ch[i].t = 0.0f; as->ch[i].dur = dur;
}

void anim_update(AnimSys *as, float dt){
    for (int i = 0; i < as->n; ){
        as->ch[i].t += dt;
        if (as->ch[i].t >= as->ch[i].dur)
            as->ch[i] = as->ch[--as->n];       /* swap-and-pop, order free */
        else i++;
    }
}

float anim_value(const AnimSys *as, int kind, int id){
    int i = find_ch(as, kind, id);
    if (i < 0) return 0.0f;
    float v = 1.0f - as->ch[i].t / as->ch[i].dur;
    return v > 0.0f ? v : 0.0f;
}

int anim_count(const AnimSys *as){ return as->n; }
