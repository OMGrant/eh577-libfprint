/* eg_engine_shim.c — forwards the ft_engine_* ABI to the file-backed engine .so
 * (eh577-engine.so), which maps the vendor matcher code FILE-BACKED (SELinux
 * 'file execute', not 'execmem'). Compiled into the driver in place of eg_engine.c;
 * eh577.c is unchanged. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
static void *H;
static int      (*p_open)(const char*);
static void     (*p_close)(void);
static void     (*p_geom)(int*,int*);
static uint32_t (*p_accept)(const uint8_t*,int,int,uint8_t);
static void     (*p_ebegin)(void);
static int      (*p_eupdate)(void);
static int      (*p_ecommit)(uint8_t**,size_t*);
static int      (*p_verify)(const uint8_t*,size_t);
__attribute__((constructor)) static void egshim_init(void){
  const char *so = getenv("EH577_ENGINE_SO");
  if(!so) so = "/usr/lib/libfprint-2/eh577-engine.so";
  H = dlopen(so, RTLD_NOW|RTLD_GLOBAL);
  if(!H){ fprintf(stderr,"eh577-shim: dlopen(%s) failed: %s\n", so, dlerror()); return; }
  p_open=dlsym(H,"ft_engine_open");     p_close=dlsym(H,"ft_engine_close");
  p_geom=dlsym(H,"ft_engine_geometry"); p_accept=dlsym(H,"ft_engine_accept");
  p_ebegin=dlsym(H,"ft_engine_enroll_begin"); p_eupdate=dlsym(H,"ft_engine_enroll_update");
  p_ecommit=dlsym(H,"ft_engine_enroll_commit"); p_verify=dlsym(H,"ft_engine_verify");
  fprintf(stderr,"eh577-shim: engine .so loaded from %s\n", so);
}
int  ft_engine_open(const char*p){ return p_open? p_open(p): -1; }
void ft_engine_close(void){ if(p_close) p_close(); }
void ft_engine_geometry(int*w,int*h){ if(p_geom) p_geom(w,h); else { if(w)*w=70; if(h)*h=57; } }
uint32_t ft_engine_accept(const uint8_t*i,int w,int h,uint8_t p){ return p_accept? p_accept(i,w,h,p): 0; }
void ft_engine_enroll_begin(void){ if(p_ebegin) p_ebegin(); }
int  ft_engine_enroll_update(void){ return p_eupdate? p_eupdate(): 1; }
int  ft_engine_enroll_commit(uint8_t**o,size_t*l){ return p_ecommit? p_ecommit(o,l): -1; }
int  ft_engine_verify(const uint8_t*t,size_t n){ return p_verify? p_verify(t,n): 0; }
