// vat_view — previewer 3D delle animazioni VAT (migrazione_3d.md §6.6).
// Context GL via SDL3 + glad. Asset bakati da vat/bake_vat.py (multi-clip + UV +
// texture). FSM per-agente + crossfade a 2 tap (frameA=clip uscente, frameB=clip
// entrante, mix=rampa) — stesso meccanismo dei prototipi soldier/debug13.html.
//
//   ./vat_view [prefix]            default prefix = vat/assets/zombie
//
// Controlli: 1=IDLE 2=WALK 3=RUN 4=ATTACK(one-shot) 5=SCREAM(one-shot)
//            C=camera SPACE=pausa N/P=clip libera (no FSM) T=texture on/off frecce=orbita
//            +/-=distanza R=reset ESC=esci
//   Texture check: G=singolo zombie <-> griglia   L=ricarica diffuse da disco
//                  Q/E=cella outfit (atlante 4x4). In singolo: tinta neutra (colori
//                  veri della texture), camera prospettica vicina, heading fisso ->
//                  orbita la camera per ispezionare ogni lato del modello.
//   Test-drive:    D = il modello cammina il loop quadrato 10x10 m (avanza con la
//                  clip walk: fase guidata dalla distanza -> piedi piantati, heading =
//                  direzione di marcia). Griglia 1 m a terra per leggere stride/slittamento.
// Headless:  VAT_VIEW_SHOT="walk"          -> fila pose del ciclo della clip
//            VAT_VIEW_SHOT="blend:idle:attack" -> fila del morph A->B (prova blend)
//            VAT_VIEW_SHOT="drive"         -> strobo del walk lungo +X sulla griglia
#include <SDL3/SDL.h>
#include <glad/glad.h>
#include "stb_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_INST 4096
#define MAX_CLIPS 64

typedef float mat4[16];
static void m_identity(mat4 m){ memset(m,0,64); m[0]=m[5]=m[10]=m[15]=1; }
static void m_mul(mat4 r,const mat4 a,const mat4 b){ mat4 t;
    for(int c=0;c<4;c++)for(int rI=0;rI<4;rI++){float s=0;for(int k=0;k<4;k++)s+=a[k*4+rI]*b[c*4+k];t[c*4+rI]=s;}
    memcpy(r,t,64);}
static void m_ortho(mat4 m,float l,float r,float b,float t,float n,float f){m_identity(m);
    m[0]=2/(r-l);m[5]=2/(t-b);m[10]=-2/(f-n);m[12]=-(r+l)/(r-l);m[13]=-(t+b)/(t-b);m[14]=-(f+n)/(f-n);}
static void m_persp(mat4 m,float fovy,float asp,float n,float f){float tf=1.0f/tanf(fovy*0.5f);
    memset(m,0,64);m[0]=tf/asp;m[5]=tf;m[10]=(f+n)/(n-f);m[11]=-1;m[14]=2*f*n/(n-f);}
static void v_sub(float*r,const float*a,const float*b){r[0]=a[0]-b[0];r[1]=a[1]-b[1];r[2]=a[2]-b[2];}
static void v_norm(float*v){float l=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);if(l>1e-9f){v[0]/=l;v[1]/=l;v[2]/=l;}}
static void v_cross(float*r,const float*a,const float*b){r[0]=a[1]*b[2]-a[2]*b[1];r[1]=a[2]*b[0]-a[0]*b[2];r[2]=a[0]*b[1]-a[1]*b[0];}
static void m_lookat(mat4 m,const float*eye,const float*ctr,const float*up){float f[3],s[3],u[3];
    v_sub(f,ctr,eye);v_norm(f);v_cross(s,f,up);v_norm(s);v_cross(u,s,f);m_identity(m);
    m[0]=s[0];m[4]=s[1];m[8]=s[2];m[1]=u[0];m[5]=u[1];m[9]=u[2];m[2]=-f[0];m[6]=-f[1];m[10]=-f[2];
    m[12]=-(s[0]*eye[0]+s[1]*eye[1]+s[2]*eye[2]);m[13]=-(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2]);
    m[14]=(f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2]);}

static char* read_file(const char*p){FILE*f=fopen(p,"rb");if(!f){fprintf(stderr,"no file %s\n",p);return NULL;}
    fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);char*b=malloc(n+1);if(fread(b,1,n,f)!=(size_t)n){}b[n]=0;fclose(f);return b;}
static GLuint mk_shader(const char*vsp,const char*fsp){char*vs=read_file(vsp),*fs=read_file(fsp);if(!vs||!fs)return 0;
    GLint ok;char log[1024];
    GLuint v=glCreateShader(GL_VERTEX_SHADER);glShaderSource(v,1,(const char**)&vs,0);glCompileShader(v);
    glGetShaderiv(v,GL_COMPILE_STATUS,&ok);if(!ok){glGetShaderInfoLog(v,1024,0,log);fprintf(stderr,"VS:%s\n",log);}
    GLuint fr=glCreateShader(GL_FRAGMENT_SHADER);glShaderSource(fr,1,(const char**)&fs,0);glCompileShader(fr);
    glGetShaderiv(fr,GL_COMPILE_STATUS,&ok);if(!ok){glGetShaderInfoLog(fr,1024,0,log);fprintf(stderr,"FS:%s\n",log);}
    GLuint p=glCreateProgram();glAttachShader(p,v);glAttachShader(p,fr);glLinkProgram(p);
    glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok){glGetProgramInfoLog(p,1024,0,log);fprintf(stderr,"LINK:%s\n",log);}
    free(vs);free(fs);glDeleteShader(v);glDeleteShader(fr);return p;}
static GLuint mk_shader_src(const char*vs,const char*fs){GLint ok;char log[1024];
    GLuint v=glCreateShader(GL_VERTEX_SHADER);glShaderSource(v,1,&vs,0);glCompileShader(v);
    glGetShaderiv(v,GL_COMPILE_STATUS,&ok);if(!ok){glGetShaderInfoLog(v,1024,0,log);fprintf(stderr,"gVS:%s\n",log);}
    GLuint fr=glCreateShader(GL_FRAGMENT_SHADER);glShaderSource(fr,1,&fs,0);glCompileShader(fr);
    glGetShaderiv(fr,GL_COMPILE_STATUS,&ok);if(!ok){glGetShaderInfoLog(fr,1024,0,log);fprintf(stderr,"gFS:%s\n",log);}
    GLuint p=glCreateProgram();glAttachShader(p,v);glAttachShader(p,fr);glLinkProgram(p);
    glDeleteShader(v);glDeleteShader(fr);return p;}
static GLuint tex_raw(const char*p,int w,int h){char*d=read_file(p);if(!d)return 0;
    GLuint t;glGenTextures(1,&t);glBindTexture(GL_TEXTURE_2D,t);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGB32F,w,h,0,GL_RGBA,GL_FLOAT,d);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);free(d);return t;}
static GLuint tex_png(const char*p){int w,h,n; // V-flip gestito nello shader (atlante outfit)
    unsigned char*d=stbi_load(p,&w,&h,&n,0);if(!d){printf("no png %s (flat shading)\n",p);return 0;}
    GLuint t;glGenTextures(1,&t);glBindTexture(GL_TEXTURE_2D,t);
    GLenum fmt=(n==4)?GL_RGBA:GL_RGB;glTexImage2D(GL_TEXTURE_2D,0,fmt,w,h,0,fmt,GL_UNSIGNED_BYTE,d);glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    stbi_image_free(d);printf("texture %s %dx%d\n",p,w,h);return t;}

typedef struct { char name[32]; int startFrame,numFrames; float duration,stride; } Clip;
typedef struct { int texW,texH,rowsPerFrame; float fps,scale; int totalFrames; Clip clip[MAX_CLIPS]; int nclips; } Meta;
static void load_meta(const char*p,Meta*m){char*s=read_file(p);if(!s)return; char*l=s;
    while(*l){ char*nl=strchr(l,'\n'); if(nl)*nl=0;
        if(!strncmp(l,"clip=",5) && m->nclips<MAX_CLIPS){ Clip*c=&m->clip[m->nclips];
            if(sscanf(l,"clip=%31s startFrame=%d numFrames=%d duration_s=%f stride_m=%f",
                      c->name,&c->startFrame,&c->numFrames,&c->duration,&c->stride)>=3) m->nclips++;
        } else { char*eq=strchr(l,'='); if(eq){*eq=0;char*k=l,*v=eq+1;
            if(!strcmp(k,"texW"))m->texW=atoi(v); else if(!strcmp(k,"texH"))m->texH=atoi(v);
            else if(!strcmp(k,"rowsPerFrame"))m->rowsPerFrame=atoi(v); else if(!strcmp(k,"fps"))m->fps=atof(v);
            else if(!strcmp(k,"scale"))m->scale=atof(v); else if(!strcmp(k,"totalFrames"))m->totalFrames=atoi(v);} }
        if(!nl)break;
        l=nl+1; } free(s);}
static int clip_by_name(Meta*m,const char*n){for(int i=0;i<m->nclips;i++)if(!strcmp(m->clip[i].name,n))return i;return -1;}

static void save_bmp(const char*path,int w,int h,unsigned char*rgb){int row=(w*3+3)&~3,sz=row*h;unsigned char hdr[54]={0};
    hdr[0]='B';hdr[1]='M';int fsz=54+sz;memcpy(hdr+2,&fsz,4);int off=54;memcpy(hdr+10,&off,4);int hs=40;memcpy(hdr+14,&hs,4);
    memcpy(hdr+18,&w,4);memcpy(hdr+22,&h,4);short pl=1,bpp=24;memcpy(hdr+26,&pl,2);memcpy(hdr+28,&bpp,2);memcpy(hdr+34,&sz,4);
    FILE*f=fopen(path,"wb");fwrite(hdr,1,54,f);unsigned char*line=calloc(row,1);
    for(int y=0;y<h;y++){for(int x=0;x<w;x++){unsigned char*px=rgb+(y*w+x)*3;line[x*3]=px[2];line[x*3+1]=px[1];line[x*3+2]=px[0];}fwrite(line,1,row,f);}
    free(line);fclose(f);}

// ---------- FSM per-agente (logica CPU; nel gioco vivrà nel render layer) ----------
enum { ST_IDLE, ST_WALK, ST_RUN, ST_ATTACK, ST_SCREAM, ST_N };
static const char* st_prefix[ST_N] = {"idle","walk","run","attack","scream"};
static int st_oneshot(int s){ return s==ST_ATTACK || s==ST_SCREAM; }
typedef struct { int idx[16]; int n; } Group;
static Group groups[ST_N];
static void build_groups(Meta*m){ for(int s=0;s<ST_N;s++)groups[s].n=0;
    for(int i=0;i<m->nclips;i++) for(int s=0;s<ST_N;s++)
        if(!strncmp(m->clip[i].name,st_prefix[s],strlen(st_prefix[s]))){ if(groups[s].n<16)groups[s].idx[groups[s].n++]=i; break; } }
static unsigned hashu(unsigned a){a^=a>>16;a*=2654435761u;a^=a>>13;return a;}
static int pick_variant(int id,int st){Group*g=&groups[st];return g->n?g->idx[hashu(id*131u+st*977u)%g->n]:0;}

typedef struct { int state,clipA; float timeA; int blending,clipB,targetState; float timeB,blendF; int prevState; } Agent;
static Agent agents[MAX_INST];
#define BLEND_DUR 0.25f

static void agent_transition(Agent*a,int id,int ns){
    if(groups[ns].n==0) return;                       // stato senza clip: ignora
    if(a->blending && a->targetState==ns) return;
    if(!st_oneshot(a->state) && !a->blending) a->prevState=a->state; // ricorda stato base
    a->blending=1; a->targetState=ns; a->clipB=pick_variant(id,ns); a->timeB=0; a->blendF=0;
}
static void agent_update(Agent*a,int id,Meta*m,float dt){
    float fps=m->fps;
    if(a->blending){
        a->timeA+=dt*fps; a->timeB+=dt*fps; a->blendF+=dt/BLEND_DUR;
        if(a->blendF>=1.0f){ a->clipA=a->clipB; a->timeA=a->timeB; a->blending=0; a->state=a->targetState; }
    } else {
        Clip*c=&m->clip[a->clipA]; a->timeA+=dt*fps;
        if(st_oneshot(a->state)){ if(a->timeA>=c->numFrames-1) agent_transition(a,id,a->prevState); } // azione finita -> ritorno
        else a->timeA=fmodf(a->timeA,(float)c->numFrames);
    }
}
static void agent_frames(Agent*a,Meta*m,float*gA,float*gB,float*mix){
    Clip*ca=&m->clip[a->clipA];
    if(a->blending){ Clip*cb=&m->clip[a->clipB];
        float la=fmodf(floorf(a->timeA),(float)ca->numFrames), lb=fmodf(floorf(a->timeB),(float)cb->numFrames);
        *gA=ca->startFrame+la; *gB=cb->startFrame+lb; *mix=a->blendF>1?1:a->blendF;
    } else {
        float local=fmodf(a->timeA,(float)ca->numFrames); if(local<0)local+=ca->numFrames;
        float fa=floorf(local),fb=fa+1; if(fb>=ca->numFrames)fb=0;
        *gA=ca->startFrame+fa; *gB=ca->startFrame+fb; *mix=local-fa;
    }
}

static float inst_buf[MAX_INST*12]; static int inst_count=0;
static void inst_push(float x,float y,float z,float head,float scale,float gA,float gB,float mix,float outfit,float tr,float tg,float tb){
    if(inst_count>=MAX_INST)return;
    float*o=inst_buf+inst_count*12;
    o[0]=x;o[1]=y;o[2]=z;o[3]=head;o[4]=scale;o[5]=gA;o[6]=gB;o[7]=mix;o[8]=outfit;o[9]=tr;o[10]=tg;o[11]=tb;inst_count++;}

int main(int argc,char**argv){
    const char*prefix=argc>1?argv[1]:"vat/assets/zombie";
    char meshp[512],posp[512],normp[512],metap[512],diffp[512];
    snprintf(meshp,512,"%s_mesh.bin",prefix);snprintf(posp,512,"%s_pos.raw",prefix);
    snprintf(normp,512,"%s_norm.raw",prefix);snprintf(metap,512,"%s_meta.txt",prefix);snprintf(diffp,512,"%s_diffuse.png",prefix);

    Meta meta={0}; load_meta(metap,&meta);
    if(meta.nclips<=0){fprintf(stderr,"meta non letto (%s)\n",metap);return 1;}
    build_groups(&meta);
    printf("meta: tex %dx%d rows/frame=%d clips=%d scale=%.5f\n",meta.texW,meta.texH,meta.rowsPerFrame,meta.nclips,meta.scale);
    printf("gruppi FSM: idle=%d walk=%d run=%d attack=%d scream=%d\n",
           groups[ST_IDLE].n,groups[ST_WALK].n,groups[ST_RUN].n,groups[ST_ATTACK].n,groups[ST_SCREAM].n);

    // --- modalità shot ---
    const char*shot=getenv("VAT_VIEW_SHOT");
    int shot_blend=0, shot_single=0, shot_drive=0, blendA=0, blendB=0, shotClip=0;
    if(shot && !strncmp(shot,"blend",5)){ shot_blend=1;
        char a[32]="idle",b[32]="attack"; sscanf(shot,"blend:%31[^:]:%31s",a,b);
        int ia=clip_by_name(&meta,a),ib=clip_by_name(&meta,b); blendA=ia<0?0:ia; blendB=ib<0?0:ib;
    } else if(shot && !strncmp(shot,"single",6)){ shot_single=1;   // singolo zombie centrato
        char c[32]="idle"; sscanf(shot,"single:%31s",c); int ic=clip_by_name(&meta,c); shotClip=ic<0?0:ic;
    } else if(shot && !strncmp(shot,"drive",5)){ shot_drive=1;      // strobo: walk lungo +X sulla griglia
    } else if(shot && shot[0]){ int c=clip_by_name(&meta,shot); shotClip=c<0?0:c; }
    int SW=1280,SH=720;
    if(shot_single){SW=540;SH=720;} else if(shot_drive){SW=1100;SH=420;} else if(shot){SW=1000;SH=360;}

    if(!SDL_Init(SDL_INIT_VIDEO)){fprintf(stderr,"SDL:%s\n",SDL_GetError());return 1;}
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,24);
    SDL_Window*win=SDL_CreateWindow("vat_view",SW,SH,SDL_WINDOW_OPENGL);
    if(!win){fprintf(stderr,"win:%s\n",SDL_GetError());return 1;}
    SDL_GLContext ctx=SDL_GL_CreateContext(win);
    if(!ctx){fprintf(stderr,"glctx:%s\n",SDL_GetError());return 1;}
    if(!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)){fprintf(stderr,"glad fail\n");return 1;}
    printf("GL %s\n",glGetString(GL_VERSION));

    FILE*mf=fopen(meshp,"rb");if(!mf){fprintf(stderr,"no %s\n",meshp);return 1;}
    int nv,ni;if(fread(&nv,4,1,mf)){}if(fread(&ni,4,1,mf)){}
    float*verts=malloc(nv*3*4),*uvs=malloc(nv*2*4);unsigned short*idx=malloc(ni*2);
    if(fread(verts,4,nv*3,mf)){}if(fread(uvs,4,nv*2,mf)){}if(fread(idx,2,ni,mf)){}fclose(mf);
    printf("mesh: %d verts %d tris\n",nv,ni/3);

    GLuint prog=mk_shader("vat/vat.vs","vat/vat.fs");
    GLuint texP=tex_raw(posp,meta.texW,meta.texH),texN=tex_raw(normp,meta.texW,meta.texH);
    GLuint texD=tex_png(diffp); int hasTex=texD!=0, useTex=hasTex;

    GLuint vao,vbo_p,vbo_uv,ebo,vbo_i;
    glGenVertexArrays(1,&vao);glBindVertexArray(vao);
    glGenBuffers(1,&vbo_p);glBindBuffer(GL_ARRAY_BUFFER,vbo_p);glBufferData(GL_ARRAY_BUFFER,nv*3*4,verts,GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,0,0);glEnableVertexAttribArray(0);
    glGenBuffers(1,&vbo_uv);glBindBuffer(GL_ARRAY_BUFFER,vbo_uv);glBufferData(GL_ARRAY_BUFFER,nv*2*4,uvs,GL_STATIC_DRAW);
    glVertexAttribPointer(1,2,GL_FLOAT,0,0,0);glEnableVertexAttribArray(1);
    glGenBuffers(1,&ebo);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo);glBufferData(GL_ELEMENT_ARRAY_BUFFER,ni*2,idx,GL_STATIC_DRAW);
    glGenBuffers(1,&vbo_i);glBindBuffer(GL_ARRAY_BUFFER,vbo_i);glBufferData(GL_ARRAY_BUFFER,sizeof(inst_buf),NULL,GL_DYNAMIC_DRAW);
    for(int i=0;i<3;i++){glEnableVertexAttribArray(2+i);
        glVertexAttribPointer(2+i,4,GL_FLOAT,0,12*sizeof(float),(void*)(i*4*sizeof(float)));glVertexAttribDivisor(2+i,1);}
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);

    // --- griglia a terra (riferimento per la modalità test-drive) ---
    // linee XZ ogni 1 m da -2 a 12: serve a vedere se i piedi slittano (stride)
    // e a leggere i 10 m di ogni lato del loop.
    float gridv[64*4*3]; int gn=0;
    for(int i=-2;i<=12;i++){
        gridv[gn++]=i; gridv[gn++]=0; gridv[gn++]=-2;  gridv[gn++]=i; gridv[gn++]=0; gridv[gn++]=12;
        gridv[gn++]=-2; gridv[gn++]=0; gridv[gn++]=i;  gridv[gn++]=12; gridv[gn++]=0; gridv[gn++]=i;
    }
    int gridVerts=gn/3;
    GLuint gvao,gvbo;
    glGenVertexArrays(1,&gvao);glBindVertexArray(gvao);
    glGenBuffers(1,&gvbo);glBindBuffer(GL_ARRAY_BUFFER,gvbo);glBufferData(GL_ARRAY_BUFFER,gn*4,gridv,GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,0,0);glEnableVertexAttribArray(0);glBindVertexArray(0);
    GLuint gprog=mk_shader_src(
        "#version 330 core\nlayout(location=0) in vec3 p;uniform mat4 uVP;void main(){gl_Position=uVP*vec4(p,1.0);}",
        "#version 330 core\nout vec4 c;uniform vec3 col;void main(){c=vec4(col,1.0);}");
    GLint uGVP=glGetUniformLocation(gprog,"uVP"),uGcol=glGetUniformLocation(gprog,"col");

    // --- stato modalità test-drive (loop quadrato 10x10 m) ---
    int wclip = groups[ST_WALK].n ? groups[ST_WALK].idx[0] : 0;  // prima variante walk
    const float LEG=10.0f;
    const float legdir[4][2]={{1,0},{0,1},{-1,0},{0,-1}};        // +X,+Z,-X,-Z
    const float corner[4][2]={{0,0},{10,0},{10,10},{0,10}};
    int drive=0, dleg=0; float dlegdist=0, dphase=0;

    GLint uVP=glGetUniformLocation(prog,"uVP"),uTS=glGetUniformLocation(prog,"texSize"),uRPF=glGetUniformLocation(prog,"rowsPerFrame");
    GLint uHas=glGetUniformLocation(prog,"uHasTex");
    float pal[8][3]={{0.45,0.55,0.40},{0.50,0.58,0.42},{0.40,0.50,0.38},{0.55,0.55,0.45},
                     {0.42,0.52,0.46},{0.48,0.50,0.40},{0.38,0.48,0.42},{0.52,0.56,0.44}};

    // --- griglia FSM (modalità interattiva) ---
    int G=8, NA=G*G; float SP=1.4f;
    for(int id=0;id<NA;id++){ agents[id].state=ST_WALK; agents[id].clipA=pick_variant(id,ST_WALK);
        agents[id].timeA=hashu(id)%60; agents[id].blending=0; agents[id].prevState=ST_IDLE; }
    int clipLibero=shotClip; // N/P liberi (preview clip senza FSM)
    int useFSM=1;

    int cam_free=0;float az=0.6f,el=0.45f,dist=18.0f;float cx=0,cy=0.9f,cz=0;int paused=0;
    int K=6;
    int single=0, curOutfit=0;   // modalità ispezione texture (singolo zombie)
    Uint64 last=SDL_GetTicks();int running=1,shot_done=0;
    while(running){
        SDL_Event e;
        while(SDL_PollEvent(&e)){
            if(e.type==SDL_EVENT_QUIT)running=0;
            else if(e.type==SDL_EVENT_KEY_DOWN)switch(e.key.key){
                case SDLK_ESCAPE:running=0;break; case SDLK_C:cam_free=!cam_free;break; case SDLK_SPACE:paused=!paused;break;
                case SDLK_1:case SDLK_2:case SDLK_3:case SDLK_4:case SDLK_5:{
                    int ns=e.key.key-SDLK_1; useFSM=1;
                    for(int id=0;id<NA;id++)agent_transition(&agents[id],id,ns);
                    printf("-> %s\n",st_prefix[ns]); break; }
                case SDLK_N:useFSM=0;clipLibero=(clipLibero+1)%meta.nclips;printf("clip libera: %s\n",meta.clip[clipLibero].name);break;
                case SDLK_P:useFSM=0;clipLibero=(clipLibero+meta.nclips-1)%meta.nclips;printf("clip libera: %s\n",meta.clip[clipLibero].name);break;
                case SDLK_T:useTex=hasTex?!useTex:0;break;
                case SDLK_G: single=!single;             // singolo zombie <-> griglia
                    if(single){cam_free=1;dist=3.5f;el=0.20f;az=0.0f;}else{cam_free=0;dist=18;el=0.45f;az=0.6f;}
                    printf("modalità: %s\n",single?"SINGOLO (texture check)":"griglia"); break;
                case SDLK_D: drive=!drive;               // test-drive: cammina il loop 10x10 m
                    if(drive){ dleg=0;dlegdist=0;dphase=0; cam_free=1;dist=22;el=0.40f;az=0.7f;
                        float sp=meta.clip[wclip].duration>0?meta.clip[wclip].stride/meta.clip[wclip].duration:0;
                        printf("modalità: TEST-DRIVE (walk %s, stride=%.2fm, v=%.2f m/s)\n",
                               meta.clip[wclip].name,meta.clip[wclip].stride,sp); }
                    else { cam_free=0;dist=18;el=0.45f;az=0.6f; printf("modalità: griglia\n"); }
                    break;
                case SDLK_L:                             // ricarica la diffuse da disco
                    if(texD){glDeleteTextures(1,&texD);}
                    texD=tex_png(diffp); hasTex=texD!=0; useTex=hasTex;
                    printf("texture ricaricata da %s\n",diffp); break;
                case SDLK_Q: curOutfit=(curOutfit+15)%16; printf("outfit %d\n",curOutfit); break;
                case SDLK_E: curOutfit=(curOutfit+1)%16; printf("outfit %d\n",curOutfit); break;
                case SDLK_LEFT:az-=0.08f;break; case SDLK_RIGHT:az+=0.08f;break;
                case SDLK_UP:el+=0.05f;break; case SDLK_DOWN:el-=0.05f;break;
                case SDLK_EQUALS:case SDLK_PLUS:dist*=0.9f;break; case SDLK_MINUS:dist*=1.1f;break;
                case SDLK_R:az=single?0.0f:0.6f;el=single?0.20f:0.45f;dist=single?3.5f:18;break;
            }
        }
        Uint64 now=SDL_GetTicks();float dt=(now-last)/1000.0f;last=now; if(dt>0.1f)dt=0.1f;

        inst_count=0;
        if(shot_single){                     // singolo zombie centrato (verifica texture headless)
            cam_free=1; az=0.0f; el=0.18f; dist=3.6f; cx=0; cy=0.9f; cz=0;
            Clip*c=&meta.clip[shotClip]; float fa=floorf(c->numFrames*0.4f),fb=fa+1; if(fb>=c->numFrames)fb=0;
            inst_push(0,0,0,0.0f,1.0f,c->startFrame+fa,c->startFrame+fb,0.0f,(float)curOutfit,1.0f,1.0f,1.0f);
        } else if(shot_blend){               // fila: morph congelato A -> B (prova crossfade)
            az=0.6f;el=0.42f;cx=0;cy=0.9f;cz=0;dist=30; float sp=1.3f,head=az;
            Clip*ca=&meta.clip[blendA],*cb=&meta.clip[blendB];
            for(int i=0;i<K;i++){ float x=(i-(K-1)/2.0f)*sp; float t=(K>1)?(float)i/(K-1):0;
                inst_push(x,0,0,head,1.0f,(float)ca->startFrame,(float)cb->startFrame,t,0,pal[i%8][0],pal[i%8][1],pal[i%8][2]); }
        } else if(shot_drive){               // strobo: walk lungo +X sulla griglia (verifica stride+heading)
            cam_free=1; cx=4.5f; cy=0.9f; cz=0; az=3.14159f; el=0.18f; dist=12;
            Clip*c=&meta.clip[wclip]; float st=c->stride>0.1f?c->stride:1.0f; int N=10;
            for(int i=0;i<N;i++){ float x=i*1.0f; float ph=x/st; ph-=floorf(ph);
                float local=ph*c->numFrames; float fa=floorf(local),fb=fa+1; if(fb>=c->numFrames)fb=0;
                inst_push(x,0,0, 1.5708f, 1.0f, c->startFrame+fa, c->startFrame+fb, local-fa, 0, 1,1,1); }
        } else if(shot){                     // fila: pose del ciclo di una clip
            az=0.6f;el=0.42f;cx=0;cy=0.9f;cz=0;dist=30; float sp=1.3f,head=az; Clip*c=&meta.clip[shotClip];
            for(int i=0;i<K;i++){ float x=(i-(K-1)/2.0f)*sp; float local=(float)i/K*c->numFrames;
                float fa=floorf(local),fb=fa+1; if(fb>=c->numFrames)fb=0;
                inst_push(x,0,0,head,1.0f,c->startFrame+fa,c->startFrame+fb,local-fa,(float)(i%16),pal[i%8][0],pal[i%8][1],pal[i%8][2]); }
        } else if(drive){                    // TEST-DRIVE: cammina il loop 10x10 m (stride+heading)
            cx=5;cy=1.0f;cz=5;               // camera centrata sul quadrato
            Clip*c=&meta.clip[wclip]; float st=c->stride>0.1f?c->stride:1.0f;
            float sp=c->duration>0? c->stride/c->duration : 0.5f;   // velocità naturale del walk
            if(!paused){ float d=sp*dt; dlegdist+=d; dphase+=d/st; dphase-=floorf(dphase);
                if(dlegdist>=LEG){ dlegdist-=LEG; dleg=(dleg+1)&3; } }
            float px=corner[dleg][0]+legdir[dleg][0]*dlegdist;
            float pz=corner[dleg][1]+legdir[dleg][1]*dlegdist;
            float head=atan2f(legdir[dleg][0],legdir[dleg][1]);
            float local=dphase*c->numFrames; float fa=floorf(local),fb=fa+1; if(fb>=c->numFrames)fb=0;
            inst_push(px,0,pz,head,1.0f,c->startFrame+fa,c->startFrame+fb,local-fa,0,1,1,1);
        } else if(single){                   // singolo zombie: ispezione texture
            cx=0;cy=0.9f;cz=0; int id=0; float gA,gB,mix;
            if(useFSM){ if(!paused)agent_update(&agents[id],id,&meta,dt); agent_frames(&agents[id],&meta,&gA,&gB,&mix); }
            else { Clip*c=&meta.clip[clipLibero];
                float local=fmodf((float)(now/1000.0)*meta.fps,(float)c->numFrames);
                float fa=floorf(local),fb=fa+1; if(fb>=c->numFrames)fb=0; gA=c->startFrame+fa;gB=c->startFrame+fb;mix=local-fa; }
            // heading fisso a 0 (la camera orbita attorno) + tinta neutra (1,1,1) = colori veri
            inst_push(0,0,0,0.0f,1.0f,gA,gB,mix,(float)curOutfit,1.0f,1.0f,1.0f);
        } else {                             // griglia interattiva
            cx=0;cy=0.9f;cz=0;
            for(int gz=0;gz<G;gz++)for(int gx=0;gx<G;gx++){int id=gz*G+gx;
                float x=(gx-(G-1)/2.0f)*SP,z=(gz-(G-1)/2.0f)*SP; float head=az+((hashu(id)%100)/100.0f-0.5f)*0.6f;
                float gA,gB,mix; int pi=id%8;
                if(useFSM){ if(!paused)agent_update(&agents[id],id,&meta,dt); agent_frames(&agents[id],&meta,&gA,&gB,&mix); }
                else { Clip*c=&meta.clip[clipLibero]; static float t=0; (void)t;
                    float local=fmodf((float)(now/1000.0)*meta.fps+id*3.1f,(float)c->numFrames);
                    float fa=floorf(local),fb=fa+1; if(fb>=c->numFrames)fb=0; gA=c->startFrame+fa;gB=c->startFrame+fb;mix=local-fa; }
                inst_push(x,0,z,head,1.0f,gA,gB,mix,(float)(hashu(id+777u)%16),pal[pi][0],pal[pi][1],pal[pi][2]); }
        }
        glBindBuffer(GL_ARRAY_BUFFER,vbo_i);glBufferSubData(GL_ARRAY_BUFFER,0,inst_count*12*sizeof(float),inst_buf);

        float asp=(float)SW/SH;mat4 proj,view,vp;float ctr[3]={cx,cy,cz},up[3]={0,1,0};
        float eye[3]={cx+dist*cosf(el)*sinf(az),cy+dist*sinf(el),cz+dist*cosf(el)*cosf(az)};
        if(cam_free)m_persp(proj,45.0f*3.14159f/180.0f,asp,0.1f,200.0f);
        else{float hh=shot?1.5f:7.0f;m_ortho(proj,-hh*asp,hh*asp,-hh,hh,-100,200);}
        m_lookat(view,eye,ctr,up);m_mul(vp,proj,view);

        glViewport(0,0,SW,SH);glClearColor(0.10f,0.11f,0.14f,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        if(drive||shot_drive){ glUseProgram(gprog);glUniformMatrix4fv(uGVP,1,GL_FALSE,vp);
            glUniform3f(uGcol,0.30f,0.32f,0.38f);glBindVertexArray(gvao);glDrawArrays(GL_LINES,0,gridVerts);glBindVertexArray(0); }
        glUseProgram(prog);glUniformMatrix4fv(uVP,1,GL_FALSE,vp);
        glUniform2f(uTS,(float)meta.texW,(float)meta.texH);glUniform1f(uRPF,(float)meta.rowsPerFrame);glUniform1i(uHas,useTex);
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,texP);glUniform1i(glGetUniformLocation(prog,"texPos"),0);
        glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,texN);glUniform1i(glGetUniformLocation(prog,"texNorm"),1);
        glActiveTexture(GL_TEXTURE2);glBindTexture(GL_TEXTURE_2D,texD);glUniform1i(glGetUniformLocation(prog,"texDiff"),2);
        glBindVertexArray(vao);glDrawElementsInstanced(GL_TRIANGLES,ni,GL_UNSIGNED_SHORT,0,inst_count);glBindVertexArray(0);

        if(shot&&!shot_done){glFinish();unsigned char*px=malloc(SW*SH*3);
            glReadPixels(0,0,SW,SH,GL_RGB,GL_UNSIGNED_BYTE,px);save_bmp("vat_view_shot.bmp",SW,SH,px);free(px);
            printf("screenshot -> vat_view_shot.bmp (%d istanze)\n",inst_count);shot_done=1;running=0;}
        SDL_GL_SwapWindow(win);
    }
    SDL_GL_DestroyContext(ctx);SDL_DestroyWindow(win);SDL_Quit();return 0;
}
