// vat_horde — l'orda reale del core sim_particles resa in 3D VAT
// (migrazione_3d.md, "agganciare al core"). Scena chokepoint: spawn dal basso,
// muro con varchi, goal in cima. Il render layer vat_layer guida heading/FSM/
// fase/outfit per slot; il renderer disegna instanced.
//
//   ./vat_horde
// Controlli: frecce=pan  +/-=zoom  C=camera  T=texture  SPACE=pausa  ESC=esci
// Headless:  VAT_HORDE_SHOT="<frames>" ./vat_horde  -> simula N step, screenshot
//            vat_horde_shot.bmp, esce (per verifica visiva).
#include <SDL3/SDL.h>
#include "vat_gl.h"
#include "vat_layer.h"
#include "sim_particles.h"

#define GW 200
#define GH 140
#define CELL 0.5f
#define MAXA 8000
static float inst[MAXA*12];

static void build_scene(SimP *s){
    int wy = (int)(GH*0.60f);                 // muro orizzontale
    for(int x=0;x<GW;x++) simp_set_wall(s,x,wy,true);
    for(int g=0; g<GW; g+=34) for(int w=0; w<7 && g+w<GW; w++) simp_set_wall(s,g+w,wy,false); // varchi
    for(int gx=GW/2-5; gx<=GW/2+5; gx++) for(int gy=GH-4; gy<=GH-2; gy++) simp_set_goal(s,gx,gy,true); // goal in cima
    simp_terrain_commit(s);
}

int main(void){
    const char *prefix="vat/assets/zombie";
    char pos[256],norm[256],mesh[256],diff[256],meta[256];
    snprintf(pos,256,"%s_pos.raw",prefix);snprintf(norm,256,"%s_norm.raw",prefix);
    snprintf(mesh,256,"%s_mesh.bin",prefix);snprintf(diff,256,"%s_diffuse.png",prefix);
    snprintf(meta,256,"%s_meta.txt",prefix);

    VatLayer *vl=vat_layer_create(meta,MAXA);
    const VatMeta *M=vat_layer_meta(vl);
    if(M->nclips<=0){fprintf(stderr,"meta vuoto\n");return 1;}
    printf("clips=%d tex %dx%d rows/frame=%d\n",M->nclips,M->texW,M->texH,M->rowsPerFrame);

    SimP *s=simp_create(GW,GH,CELL,MAXA);
    build_scene(s);

    const char *shot=getenv("VAT_HORDE_SHOT");
    int shot_frames = shot? atoi(shot):0; if(shot&&shot_frames<=0)shot_frames=600;
    int SW=1280,SH=720;

    if(!SDL_Init(SDL_INIT_VIDEO)){fprintf(stderr,"SDL:%s\n",SDL_GetError());return 1;}
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,24);
    SDL_Window*win=SDL_CreateWindow("vat_horde",SW,SH,SDL_WINDOW_OPENGL);
    SDL_GLContext ctx=SDL_GL_CreateContext(win);
    if(!ctx||!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)){fprintf(stderr,"GL init fail\n");return 1;}
    printf("GL %s\n",glGetString(GL_VERSION));

    FILE*mf=fopen(mesh,"rb"); int nv,ni; if(fread(&nv,4,1,mf)){}if(fread(&ni,4,1,mf)){}
    float*verts=malloc(nv*3*4),*uvs=malloc(nv*2*4); unsigned short*idx=malloc(ni*2);
    if(fread(verts,4,nv*3,mf)){}if(fread(uvs,4,nv*2,mf)){}if(fread(idx,2,ni,mf)){}fclose(mf);

    GLuint prog=vg_shader("vat/vat.vs","vat/vat.fs");
    GLuint texP=vg_tex_raw(pos,M->texW,M->texH),texN=vg_tex_raw(norm,M->texW,M->texH),texD=vg_tex_png(diff);
    int useTex=texD!=0;
    GLuint vao,bp,bu,eb,bi; glGenVertexArrays(1,&vao);glBindVertexArray(vao);
    glGenBuffers(1,&bp);glBindBuffer(GL_ARRAY_BUFFER,bp);glBufferData(GL_ARRAY_BUFFER,nv*3*4,verts,GL_STATIC_DRAW);
    glVertexAttribPointer(0,3,GL_FLOAT,0,0,0);glEnableVertexAttribArray(0);
    glGenBuffers(1,&bu);glBindBuffer(GL_ARRAY_BUFFER,bu);glBufferData(GL_ARRAY_BUFFER,nv*2*4,uvs,GL_STATIC_DRAW);
    glVertexAttribPointer(1,2,GL_FLOAT,0,0,0);glEnableVertexAttribArray(1);
    glGenBuffers(1,&eb);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,eb);glBufferData(GL_ELEMENT_ARRAY_BUFFER,ni*2,idx,GL_STATIC_DRAW);
    glGenBuffers(1,&bi);glBindBuffer(GL_ARRAY_BUFFER,bi);glBufferData(GL_ARRAY_BUFFER,sizeof(inst),NULL,GL_DYNAMIC_DRAW);
    for(int i=0;i<3;i++){glEnableVertexAttribArray(2+i);
        glVertexAttribPointer(2+i,4,GL_FLOAT,0,12*sizeof(float),(void*)(i*4*sizeof(float)));glVertexAttribDivisor(2+i,1);}
    glBindVertexArray(0); glEnable(GL_DEPTH_TEST);
    GLint uVP=glGetUniformLocation(prog,"uVP"),uTS=glGetUniformLocation(prog,"texSize"),uRPF=glGetUniformLocation(prog,"rowsPerFrame"),uHas=glGetUniformLocation(prog,"uHasTex");

    float cx=GW*CELL*0.5f, cz=22.0f, hh=15.0f, az=0.7f, el=0.40f; int cam_free=0,paused=0;
    { const char*cs=getenv("VAT_HORDE_CAM"); if(cs) sscanf(cs,"%f,%f,%f,%f,%f",&cx,&cz,&hh,&az,&el); }
    SimPAgentDesc runner={0.30f,3.6f,1.0f};
    unsigned rng=1234;
    Uint64 last=SDL_GetTicks(); int running=1,frame=0,shot_done=0;
    while(running){
        SDL_Event e; while(SDL_PollEvent(&e)){ if(e.type==SDL_EVENT_QUIT)running=0;
            else if(e.type==SDL_EVENT_KEY_DOWN)switch(e.key.key){
                case SDLK_ESCAPE:running=0;break; case SDLK_C:cam_free=!cam_free;break; case SDLK_T:useTex=!useTex;break;
                case SDLK_SPACE:paused=!paused;break;
                case SDLK_LEFT:az-=0.06f;break; case SDLK_RIGHT:az+=0.06f;break; case SDLK_UP:el+=0.04f;break; case SDLK_DOWN:el-=0.04f;break;
                case SDLK_EQUALS:case SDLK_PLUS:hh*=0.9f;break; case SDLK_MINUS:hh*=1.1f;break; } }
        Uint64 now=SDL_GetTicks(); (void)last; last=now;
        float dt=1.0f/60.0f;
        if(!paused){
            // spawn dal basso (burst-free via free_at), ~80% walker 20% runner
            for(int k=0;k<40 && simp_count(s)<MAXA;k++){
                rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;
                float x=2.0f+(rng%10000)/10000.0f*(GW*CELL-4.0f);
                float y=1.0f+((rng>>8)%10000)/10000.0f*5.0f;
                if(simp_free_at(s,x,y,0.34f)){ if((rng>>20)%5==0) simp_spawn_desc(s,x,y,&runner); else simp_spawn(s,x,y); }
            }
            simp_step(s,dt); vat_layer_update(vl,s,dt);
        }
        int count=vat_layer_fill(vl,s,inst,MAXA);
        glBindBuffer(GL_ARRAY_BUFFER,bi);glBufferSubData(GL_ARRAY_BUFFER,0,count*12*sizeof(float),inst);

        float asp=(float)SW/SH; mat4 proj,view,vp; float ctr[3]={cx,0.9f,cz},up[3]={0,1,0};
        float eye[3]={cx+hh*cosf(el)*sinf(az),0.9f+hh*sinf(el),cz+hh*cosf(el)*cosf(az)};
        if(cam_free)m_persp(proj,45.0f*3.14159f/180.0f,asp,0.1f,500.0f);
        else m_ortho(proj,-hh*asp,hh*asp,-hh,hh,-200,400);
        m_lookat(view,eye,ctr,up); m_mul(vp,proj,view);

        glViewport(0,0,SW,SH);glClearColor(0.12f,0.13f,0.16f,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glUseProgram(prog);glUniformMatrix4fv(uVP,1,GL_FALSE,vp);
        glUniform2f(uTS,(float)M->texW,(float)M->texH);glUniform1f(uRPF,(float)M->rowsPerFrame);glUniform1i(uHas,useTex);
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,texP);glUniform1i(glGetUniformLocation(prog,"texPos"),0);
        glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,texN);glUniform1i(glGetUniformLocation(prog,"texNorm"),1);
        glActiveTexture(GL_TEXTURE2);glBindTexture(GL_TEXTURE_2D,texD);glUniform1i(glGetUniformLocation(prog,"texDiff"),2);
        glBindVertexArray(vao);glDrawElementsInstanced(GL_TRIANGLES,ni,GL_UNSIGNED_SHORT,0,count);glBindVertexArray(0);

        frame++;
        if(shot && !shot_done && frame>=shot_frames){ glFinish();
            unsigned char*px=malloc(SW*SH*3); glReadPixels(0,0,SW,SH,GL_RGB,GL_UNSIGNED_BYTE,px);
            vg_save_bmp("vat_horde_shot.bmp",SW,SH,px); free(px);
            printf("frame %d: %d agenti, screenshot -> vat_horde_shot.bmp\n",frame,count);
            shot_done=1; running=0; }
        SDL_GL_SwapWindow(win);
    }
    vat_layer_destroy(vl); simp_destroy(s);
    SDL_GL_DestroyContext(ctx);SDL_DestroyWindow(win);SDL_Quit(); return 0;
}
