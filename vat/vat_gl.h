/* vat_gl.h — helper GL condivisi per i tool VAT (mat4, shader, texture, BMP).
 * static: incluso da un solo TU per eseguibile. stb_image: impl in stb_impl.c. */
#ifndef VAT_GL_H
#define VAT_GL_H
#include <glad/glad.h>
#include "stb_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef float mat4[16];
static void m_identity(mat4 m){ memset(m,0,64); m[0]=m[5]=m[10]=m[15]=1; }
static void m_mul(mat4 r,const mat4 a,const mat4 b){ mat4 t;
    for(int c=0;c<4;c++)for(int rI=0;rI<4;rI++){float s=0;for(int k=0;k<4;k++){s+=a[k*4+rI]*b[c*4+k];} t[c*4+rI]=s;}
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

static char* vg_read(const char*p){FILE*f=fopen(p,"rb");if(!f){fprintf(stderr,"no file %s\n",p);return NULL;}
    fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);char*b=malloc(n+1);if(fread(b,1,n,f)!=(size_t)n){}b[n]=0;fclose(f);return b;}
static GLuint vg_shader(const char*vsp,const char*fsp){char*vs=vg_read(vsp),*fs=vg_read(fsp);if(!vs||!fs)return 0;
    GLint ok;char log[1024];
    GLuint v=glCreateShader(GL_VERTEX_SHADER);glShaderSource(v,1,(const char**)&vs,0);glCompileShader(v);
    glGetShaderiv(v,GL_COMPILE_STATUS,&ok);if(!ok){glGetShaderInfoLog(v,1024,0,log);fprintf(stderr,"VS:%s\n",log);}
    GLuint fr=glCreateShader(GL_FRAGMENT_SHADER);glShaderSource(fr,1,(const char**)&fs,0);glCompileShader(fr);
    glGetShaderiv(fr,GL_COMPILE_STATUS,&ok);if(!ok){glGetShaderInfoLog(fr,1024,0,log);fprintf(stderr,"FS:%s\n",log);}
    GLuint p=glCreateProgram();glAttachShader(p,v);glAttachShader(p,fr);glLinkProgram(p);
    glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok){glGetProgramInfoLog(p,1024,0,log);fprintf(stderr,"LINK:%s\n",log);}
    free(vs);free(fs);glDeleteShader(v);glDeleteShader(fr);return p;}
static GLuint vg_tex_raw(const char*p,int w,int h){char*d=vg_read(p);if(!d)return 0;
    GLuint t;glGenTextures(1,&t);glBindTexture(GL_TEXTURE_2D,t);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGB32F,w,h,0,GL_RGBA,GL_FLOAT,d);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);free(d);return t;}
static GLuint vg_tex_png(const char*p){int w,h,n; unsigned char*d=stbi_load(p,&w,&h,&n,0);
    if(!d){printf("no png %s (flat)\n",p);return 0;}
    GLuint t;glGenTextures(1,&t);glBindTexture(GL_TEXTURE_2D,t);
    GLenum fmt=(n==4)?GL_RGBA:GL_RGB;glTexImage2D(GL_TEXTURE_2D,0,fmt,w,h,0,fmt,GL_UNSIGNED_BYTE,d);glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    stbi_image_free(d);printf("texture %s %dx%d\n",p,w,h);return t;}
static void vg_save_bmp(const char*path,int w,int h,unsigned char*rgb){int row=(w*3+3)&~3,sz=row*h;unsigned char hdr[54]={0};
    hdr[0]='B';hdr[1]='M';int fsz=54+sz;memcpy(hdr+2,&fsz,4);int off=54;memcpy(hdr+10,&off,4);int hs=40;memcpy(hdr+14,&hs,4);
    memcpy(hdr+18,&w,4);memcpy(hdr+22,&h,4);short pl=1,bpp=24;memcpy(hdr+26,&pl,2);memcpy(hdr+28,&bpp,2);memcpy(hdr+34,&sz,4);
    FILE*f=fopen(path,"wb");fwrite(hdr,1,54,f);unsigned char*line=calloc(row,1);
    for(int y=0;y<h;y++){for(int x=0;x<w;x++){unsigned char*px=rgb+(y*w+x)*3;line[x*3]=px[2];line[x*3+1]=px[1];line[x*3+2]=px[0];}fwrite(line,1,row,f);}
    free(line);fclose(f);}
#endif
