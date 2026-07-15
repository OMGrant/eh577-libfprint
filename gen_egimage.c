/* gen_egimage.c — produce the page-aligned engine image to embed in eh577-engine.so.
 * Copies PE headers+sections to their virtual offsets, bakes in the 2-byte Attach
 * patches (in .text), leaves the IAT UNRESOLVED (patched at runtime per-process). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
static u8 *g_file; static long g_filelen;
static u32 f32(u64 o){ return *(u32*)(g_file+o); }
static u16 f16(u64 o){ return *(u16*)(g_file+o); }
int main(int argc,char**argv){
  const char*in=argc>1?argv[1]:"windows-driver/EgisTouchFPEngine0577.dll";
  const char*out=argc>2?argv[2]:"egimage.bin";
  FILE*f=fopen(in,"rb"); if(!f){perror("open dll");return 1;}
  fseek(f,0,SEEK_END); g_filelen=ftell(f); fseek(f,0,SEEK_SET);
  g_file=malloc(g_filelen); if(fread(g_file,1,g_filelen,f)!=(size_t)g_filelen){perror("read");return 1;} fclose(f);
  u32 e=f32(0x3c);
  u32 sizeofimage=f32(e+24+56);
  u16 nsec=f16(e+6); u16 optsize=f16(e+20); u64 sectbl=e+24+optsize;
  u32 hdrsize=f32(e+24+60);
  u8*img=calloc(1,sizeofimage);
  memcpy(img,g_file,hdrsize);
  for(int i=0;i<nsec;i++){ u64 s=sectbl+i*40;
    u32 vaddr=f32(s+12), rawsize=f32(s+16), rawptr=f32(s+20);
    memcpy(img+vaddr, g_file+rawptr, rawsize); }
  /* bake the offline Attach patches (RVA==offset at preferred base) */
  struct { u32 rva; u8 b[2]; u8 orig[2]; } P[] = {
    { 0x1a80e, {0x31,0xdb}, {0x8b,0xd8} },   /* mov ebx,eax -> xor ebx,ebx */
    { 0x1a821, {0xeb,0x16}, {0x75,0x16} },   /* jne -> jmp */
  };
  for(unsigned i=0;i<sizeof P/sizeof*P;i++){
    if(memcmp(img+P[i].rva,P[i].orig,2)==0){ memcpy(img+P[i].rva,P[i].b,2);
      printf("[gen] baked patch rva %#x: %02x %02x -> %02x %02x\n",P[i].rva,P[i].orig[0],P[i].orig[1],P[i].b[0],P[i].b[1]); }
    else printf("[gen] WARN patch rva %#x: unexpected bytes %02x %02x\n",P[i].rva,img[P[i].rva],img[P[i].rva+1]);
  }
  FILE*o=fopen(out,"wb"); fwrite(img,1,sizeofimage,o); fclose(o);
  printf("[gen] wrote %s: %u bytes (sizeofimage), hdrsize=%u nsec=%u\n",out,sizeofimage,hdrsize,nsec);
  return 0;
}
