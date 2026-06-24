/* fx_particles.c — see fx_particles.h. Zero-dep (libm only), deterministic. */
#include "fx_particles.h"
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* --- deterministic RNG (xorshift32), no rand() ------------------------- */
static unsigned xs(unsigned *s){ unsigned x=*s; x^=x<<13; x^=x>>17; x^=x<<5; return *s=x; }
static float rf(unsigned *s){ return (float)(xs(s)>>8) * (1.0f/16777216.0f); } /* [0,1) */
static float rrange(unsigned *s, float lo, float hi){ return lo + rf(s)*(hi-lo); }
static int   rirange(unsigned *s, int lo, int hi){ return (hi<=lo)?lo : lo + (int)(xs(s)%(unsigned)(hi-lo+1)); }

void fx_init(FxParticles *fp, unsigned seed){
    memset(fp, 0, sizeof *fp);
    fp->rng = seed ? seed : 0x9e3779b9u;
}

int fx_active_count(const FxParticles *fp){
    int n=0; for(int i=0;i<FX_MAX_PARTICLES;i++) if(fp->pool[i].active) n++;
    return n;
}

void fx_update(FxParticles *fp, float dt, const float wind[2],
               FxGroundFn ground_fn, void *ground_ud){
    float wx = wind ? wind[0] : 0.0f, wz = wind ? wind[1] : 0.0f;

    for(int i=0;i<FX_MAX_PARTICLES;i++){
        FxParticle *p=&fp->pool[i];
        if(!p->active) continue;

        p->life -= dt;
        if(p->life <= 0.0f){ p->active=false; continue; }

        p->vel[1] -= p->gravity * dt;
        p->vel[0] += wx * p->wind_scale * dt;
        p->vel[2] += wz * p->wind_scale * dt;
        if(p->drag > 0.0f){
            float k = 1.0f - p->drag*dt; if(k<0.0f) k=0.0f;
            p->vel[0]*=k; p->vel[1]*=k; p->vel[2]*=k;
        }

        p->pos[0] += p->vel[0]*dt;
        p->pos[1] += p->vel[1]*dt;
        p->pos[2] += p->vel[2]*dt;

        float gy = ground_fn ? ground_fn(p->pos[0], p->pos[2], ground_ud) : 0.0f;
        float floor_y = gy + 0.01f;
        if(p->pos[1] < floor_y){
            p->pos[1] = floor_y;
            if(p->ground_stop){ p->vel[0]=p->vel[1]=p->vel[2]=0.0f; }
            else p->vel[1] = 0.0f;   /* slide */
        }
    }

    for(int i=0;i<FX_MAX_EMITTERS;i++){
        FxEmitter *ce=&fp->emitters[i];
        if(!ce->active) continue;
        ce->elapsed += dt;
        if(ce->duration >= 0.0f && ce->elapsed >= ce->duration){ ce->active=false; continue; }
        ce->timer -= dt;
        if(ce->timer <= 0.0f){
            if(ce->def){
                fx_emit(fp, ce->pos, ce->def, 0.0f, -1.0f);
                float rate = ce->def->rate > 0.0f ? ce->def->rate : 1.0f;
                ce->timer += 1.0f/rate;
            } else ce->active=false;
        }
    }
}

bool fx_emit_one(FxParticles *fp, const float pos[3], const float vel[3],
                 float life, float gravity, float drag,
                 const float start_col[4], const float end_col[4],
                 float start_scale, float end_scale, int sprite_index,
                 float wind_scale, bool ground_stop, FxBlend blend){
    for(int i=0;i<FX_MAX_PARTICLES;i++){
        FxParticle *p=&fp->pool[i];
        if(p->active) continue;
        p->active=true;
        memcpy(p->pos,pos,3*sizeof(float)); memcpy(p->vel,vel,3*sizeof(float));
        p->life=p->max_life=life;
        p->gravity=gravity; p->drag=drag;
        memcpy(p->start_color,start_col,4*sizeof(float));
        memcpy(p->end_color,end_col,4*sizeof(float));
        p->start_scale=start_scale; p->end_scale=end_scale;
        p->sprite_index=sprite_index; p->wind_scale=wind_scale;
        p->ground_stop=ground_stop; p->blend=blend;
        return true;
    }
    return false;   /* pool full: drop (oldest survive) */
}

void fx_emit(FxParticles *fp, const float origin[3], const FxEmitterDef *def,
             float direction, float half_angle){
    if(!def) return;
    unsigned *s=&fp->rng;
    bool full = half_angle < 0.0f;   /* omnidirectional */

    for(int i=0;i<def->count;i++){
        float angle = full ? rf(s)*2.0f*(float)M_PI
                           : direction + rrange(s,-half_angle,half_angle);
        float speed = rrange(s, def->speed_xy_min, def->speed_xy_max);
        float vel[3] = { cosf(angle)*speed,
                         rrange(s, def->speed_y_min, def->speed_y_max),
                         sinf(angle)*speed };

        float sx=origin[0], sz=origin[2];
        switch(def->shape){
        case FX_EMIT_POINT: break;
        case FX_EMIT_BOX:
            sx += rrange(s,-def->spawn_radius,def->spawn_radius);
            sz += rrange(s,-def->spawn_box_z,def->spawn_box_z); break;
        case FX_EMIT_LINE:
            sx += rrange(s,-def->spawn_radius,def->spawn_radius); break;
        default: { /* FX_EMIT_CIRCLE */
            float sa = full ? rf(s)*2.0f*(float)M_PI
                            : direction + rrange(s,-half_angle,half_angle);
            float sr = rf(s)*def->spawn_radius;
            sx += cosf(sa)*sr; sz += sinf(sa)*sr; break; }
        }
        float spawn[3] = { sx,
                           origin[1] + rrange(s,def->spawn_offset_y_min,def->spawn_offset_y_max),
                           sz };

        float life = rrange(s, def->lifetime_min, def->lifetime_max);
        float start_col[4], end_col[4];
        if(def->color_variant_count > 0){
            int ci = rirange(s, 0, def->color_variant_count-1);
            memcpy(start_col, def->color_variants[ci], 4*sizeof(float));
        } else memcpy(start_col, def->start_color, 4*sizeof(float));
        memcpy(end_col, def->end_color, 4*sizeof(float));

        float s0 = rrange(s, def->start_scale_min, def->start_scale_max);
        float s1 = rrange(s, def->end_scale_min,   def->end_scale_max);
        int sprite = (def->sprite_first<0) ? -1 : rirange(s, def->sprite_first, def->sprite_last);

        fx_emit_one(fp, spawn, vel, life, def->gravity, def->drag,
                    start_col, end_col, s0, s1, sprite,
                    def->wind_scale, def->ground_stop, def->blend);
    }
}

int fx_start_emitter(FxParticles *fp, const float pos[3],
                     const FxEmitterDef *def, float duration){
    for(int i=0;i<FX_MAX_EMITTERS;i++){
        if(fp->emitters[i].active) continue;
        FxEmitter *ce=&fp->emitters[i];
        ce->active=true; memcpy(ce->pos,pos,3*sizeof(float));
        ce->def=def; ce->timer=0.0f; ce->duration=duration; ce->elapsed=0.0f;
        return i;
    }
    return -1;
}
void fx_stop_emitter(FxParticles *fp, int idx){
    if(idx>=0 && idx<FX_MAX_EMITTERS) fp->emitters[idx].active=false;
}

int fx_collect(const FxParticles *fp, FxBlend blend,
               const float right[3], const float up[3],
               float *out_mat16, float *out_rgba, float *out_sprite, int max){
    /* forward = right x up (camera-facing billboard normal) */
    float fwd[3] = { right[1]*up[2]-right[2]*up[1],
                     right[2]*up[0]-right[0]*up[2],
                     right[0]*up[1]-right[1]*up[0] };
    int n=0;
    for(int i=0;i<FX_MAX_PARTICLES && n<max;i++){
        const FxParticle *p=&fp->pool[i];
        if(!p->active || p->blend!=blend) continue;

        float t = (p->max_life>0.0f) ? 1.0f-(p->life/p->max_life) : 0.0f;

        float *col=out_rgba+n*4;
        for(int c=0;c<4;c++) col[c]=p->start_color[c]+(p->end_color[c]-p->start_color[c])*t;
        out_sprite[n] = (float)p->sprite_index;

        float sc = p->start_scale+(p->end_scale-p->start_scale)*t;
        float *m=out_mat16+n*16;   /* column-major */
        m[0]=right[0]*sc; m[1]=right[1]*sc; m[2]=right[2]*sc; m[3]=0.0f;
        m[4]=up[0]*sc;    m[5]=up[1]*sc;    m[6]=up[2]*sc;    m[7]=0.0f;
        m[8]=fwd[0]*sc;   m[9]=fwd[1]*sc;   m[10]=fwd[2]*sc;  m[11]=0.0f;
        m[12]=p->pos[0];  m[13]=p->pos[1];  m[14]=p->pos[2];  m[15]=1.0f;
        n++;
    }
    return n;
}
