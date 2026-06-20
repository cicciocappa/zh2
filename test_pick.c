/* test_pick.c — headless unit test of the editor picking math (edit_pick.h).
 * EDITOR_DESIGN §6 verification (b): camera + known pixels -> world point.
 *
 * Strategy: ROUND-TRIP. Pick a known ground point (wx,0,wy), project it through
 * the same VP vat_horde builds (ortho 3/4 view AND perspective), get a pixel,
 * run pick_y0 on that pixel, assert it returns the original (wx,wy). This
 * exercises m4_invert and the plane intersection across the whole frustum
 * without hand-computing tilted geometry. Plus a couple of direct sanity checks.
 */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "vat/edit_pick.h"

/* --- tiny column-major matrix helpers, copied from vat_gl.h (kept dep-free so
 *     this test links without glad/SDL). Must match vat_horde's camera exactly. */
typedef float m4[16];
static void mi(m4 m){ memset(m,0,64); m[0]=m[5]=m[10]=m[15]=1; }
static void mmul(m4 r,const m4 a,const m4 b){ m4 t;
    for(int c=0;c<4;c++)for(int rI=0;rI<4;rI++){float s=0;for(int k=0;k<4;k++)s+=a[k*4+rI]*b[c*4+k]; t[c*4+rI]=s;}
    memcpy(r,t,64); }
static void mortho(m4 m,float l,float r,float b,float t,float n,float f){ mi(m);
    m[0]=2/(r-l);m[5]=2/(t-b);m[10]=-2/(f-n);m[12]=-(r+l)/(r-l);m[13]=-(t+b)/(t-b);m[14]=-(f+n)/(f-n);}
static void mpersp(m4 m,float fovy,float asp,float n,float f){float tf=1.0f/tanf(fovy*0.5f);
    memset(m,0,64);m[0]=tf/asp;m[5]=tf;m[10]=(f+n)/(n-f);m[11]=-1;m[14]=2*f*n/(n-f);}
static void vsub(float*r,const float*a,const float*b){r[0]=a[0]-b[0];r[1]=a[1]-b[1];r[2]=a[2]-b[2];}
static void vnorm(float*v){float l=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);if(l>1e-9f){v[0]/=l;v[1]/=l;v[2]/=l;}}
static void vcross(float*r,const float*a,const float*b){r[0]=a[1]*b[2]-a[2]*b[1];r[1]=a[2]*b[0]-a[0]*b[2];r[2]=a[0]*b[1]-a[1]*b[0];}
static void mlookat(m4 m,const float*eye,const float*ctr,const float*up){float f[3],s[3],u[3];
    vsub(f,ctr,eye);vnorm(f);vcross(s,f,up);vnorm(s);vcross(u,s,f);mi(m);
    m[0]=s[0];m[4]=s[1];m[8]=s[2];m[1]=u[0];m[5]=u[1];m[9]=u[2];m[2]=-f[0];m[6]=-f[1];m[10]=-f[2];
    m[12]=-(s[0]*eye[0]+s[1]*eye[1]+s[2]*eye[2]);m[13]=-(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2]);
    m[14]=(f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2]);}

/* world (x,0,z) -> pixel (top-left origin) through VP, like the GPU + viewport */
static void project(const m4 vp,float wx,float wz,int W,int H,float*px,float*py){
    float v[4]={wx,0,wz,1},c[4]; m4_mulv(vp,v,c);
    float nx=c[0]/c[3], ny=c[1]/c[3];
    *px=(nx+1.0f)*0.5f*W; *py=(1.0f-ny)*0.5f*H;
}

/* Build the exact VP vat_horde uses for the 3/4 ortho editor camera. */
static void build_vp_ortho(m4 vp,float cx,float cz,float hh,float az,float el,int W,int H){
    float asp=(float)W/H; m4 proj,view;
    float ctr[3]={cx,0.9f,cz},up[3]={0,1,0};
    float eye[3]={cx+hh*cosf(el)*sinf(az),0.9f+hh*sinf(el),cz+hh*cosf(el)*cosf(az)};
    mortho(proj,-hh*asp,hh*asp,-hh,hh,-200,400);
    mlookat(view,eye,ctr,up); mmul(vp,proj,view);
}
static void build_vp_persp(m4 vp,float cx,float cz,float hh,float az,float el,int W,int H){
    float asp=(float)W/H; m4 proj,view;
    float ctr[3]={cx,0.9f,cz},up[3]={0,1,0};
    float eye[3]={cx+hh*cosf(el)*sinf(az),0.9f+hh*sinf(el),cz+hh*cosf(el)*cosf(az)};
    mpersp(proj,45.0f*3.14159f/180.0f,asp,0.1f,500.0f);
    mlookat(view,eye,ctr,up); mmul(vp,proj,view);
}

static int fails=0;
static void check_roundtrip(const m4 vp,float wx,float wz,int W,int H,const char*tag){
    float px,py; project(vp,wx,wz,W,H,&px,&py);
    float rx,rz; int ok=pick_y0(vp,px,py,W,H,&rx,&rz);
    float e=fabsf(rx-wx)+fabsf(rz-wz);
    if(!ok||e>1e-2f){ printf("  FAIL %s: world(%.2f,%.2f)->px(%.1f,%.1f)->world(%.2f,%.2f) err=%.4f\n",
        tag,wx,wz,px,py,rx,rz,e); fails++; }
}

int main(void){
    int W=1280,H=720;
    /* --- round-trip over a grid of world points, ortho camera --- */
    m4 vp; build_vp_ortho(vp, 50,24, 15.0f, 0.7f, 0.40f, W,H);
    int n=0;
    for(float x=10;x<=90;x+=20) for(float z=5;z<=45;z+=10){ check_roundtrip(vp,x,z,W,H,"ortho"); n++; }
    printf("ortho round-trip: %d points\n", n);

    /* --- different ortho zoom/angle (resize-like aspect too) --- */
    build_vp_ortho(vp, 50,24, 8.0f, 1.3f, 0.55f, 1024,768);
    for(float x=30;x<=70;x+=20) for(float z=10;z<=40;z+=15){ check_roundtrip(vp,x,z,1024,768,"ortho2"); }

    /* --- perspective camera (cam_free): math must hold there too --- */
    build_vp_persp(vp, 50,24, 30.0f, 0.7f, 0.40f, W,H);
    for(float x=30;x<=70;x+=20) for(float z=10;z<=40;z+=15){ check_roundtrip(vp,x,z,W,H,"persp"); }

    /* --- direct sanity: screen center of an ortho cam looking straight down
     *     maps to the look-at ground point (cx,cz). --- */
    { m4 v2; float cx=33,cz=21;
      /* top-down: el=PI/2 -> eye straight above ctr; up must not be parallel,
       *     use az to keep lookat stable. el slightly < 90 deg. */
      build_vp_ortho(v2,cx,cz,12.0f,0.0f,1.4f,W,H);
      float rx,rz; pick_y0(v2,W/2.0f,H/2.0f,W,H,&rx,&rz);
      float e=fabsf(rx-cx)+fabsf(rz-cz);
      if(e>0.5f){ printf("  FAIL center: got(%.2f,%.2f) want~(%.0f,%.0f) err=%.3f\n",rx,rz,cx,cz,e); fails++; }
      else printf("center-pixel maps near look-at ground point (err %.3f)\n", e); }

    /* --- parallel ray: a pure top-down ortho has the ground ray perpendicular,
     *     so it DOES hit; a side-on ortho (el=0) is parallel -> must return 0. */
    { m4 v2; build_vp_ortho(v2,50,24,15.0f,0.0f,0.0f,W,H);
      float rx,rz; int ok=pick_y0(v2,W/2.0f,H/2.0f,W,H,&rx,&rz);
      if(ok){ printf("  FAIL parallel: expected miss, got hit (%.2f,%.2f)\n",rx,rz); fails++; }
      else printf("side-on (parallel) ray correctly reports no ground hit\n"); }

    if(fails){ printf("FAIL (%d)\n", fails); return 1; }
    printf("PASS\n"); return 0;
}
