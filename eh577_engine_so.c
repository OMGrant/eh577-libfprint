// loader.c — native in-process loader for ftWbioEngineAdapter.dll, NO WINE.
// Maps the PE, sets up a fake x64 TEB in %gs, runs static-CRT DllMain, then
// calls WbioQueryEngineInterface + Attach + AcceptSampleData on a real print.
//
// Build: gcc -O0 -g -o loader loader.c
// x86-64 Linux: glibc TLS uses %fs, so %gs is free for the Windows TEB.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <asm/prctl.h>
#include <link.h>    /* dl_iterate_phdr, ElfW, PT_LOAD */
#include <dlfcn.h>   /* dladdr, Dl_info */

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#define MS __attribute__((ms_abi))
#define DLL_PROCESS_ATTACH 1

static u8 *g_image;          // mapped image base
static u64 g_imagebase;      // preferred base 0x180000000
static const char *g_dllpath;

// ---------- tiny PE structures (only fields we use) ----------
#pragma pack(push,1)
typedef struct { u16 e_magic; u8 pad[58]; u32 e_lfanew; } DOS;
typedef struct {
  u32 Signature; u16 Machine, NumSecs; u32 TimeStamp, PtrSym, NumSym; u16 OptSize, Chars;
} FILEHDR;
typedef struct { u8 Name[8]; u32 VSize, VAddr, RawSize, RawPtr; u32 pad[3]; u16 pad2[2]; u32 Chars; } SEC;
#pragma pack(pop)

static u8 *g_file; static long g_filelen;
static u32 f32(u64 o){ return *(u32*)(g_file+o); }
static u64 f64(u64 o){ return *(u64*)(g_file+o); }
static u16 f16(u64 o){ return *(u16*)(g_file+o); }

// ---------- fake TEB / gs ----------
static u8 g_teb[0x2000];
static u8 g_peb[0x1000];
static void *g_tls_array[64];
static u8  g_tls_block[0x2000];

static int set_gs(void *base){ return syscall(SYS_arch_prctl, ARCH_SET_GS, base); }

// ---------- import shim table ----------
typedef struct { const char *name; void *fn; } Shim;
static Shim shims[256]; static int nshims;
static void reg(const char*n, void*f){ if(nshims>=(int)(sizeof shims/sizeof*shims)){fprintf(stderr,"ft_engine: shim table full (%d) — raise shims[]\n",nshims);abort();} shims[nshims].name=n; shims[nshims].fn=f; nshims++; }
static void* find_shim(const char*n){ for(int i=0;i<nshims;i++) if(!strcmp(shims[i].name,n)) return shims[i].fn; return 0; }

// ================= SHIMS (all MS ABI) =================
static u32 g_lasterr=0;
static u64 g_last_io_len=0;
static void MS sh_SetLastError(u32 e){ g_lasterr=e; }
static u32  MS sh_GetLastError(void){ return g_lasterr; }

static void* MS sh_GetProcessHeap(void){ return (void*)0x100; }
/* tracked, leak-only heap: free is a no-op so the static CRT can never double-free
 * or use-after-free through our allocator (which glibc would abort on). */
/* tracked-idempotent allocator (daemon-safe): frees for real so a long-running
 * fprintd daemon stays flat, but a live-pointer set makes double/foreign frees
 * no-ops (never dereferenced) so the Egis static CRT's double-frees can't crash
 * us. Replaces the old leak-only heap, which leaked ~2.7MB/verify (would OOM the
 * daemon). Single-threaded: engine threads are stubbed, so no locking needed. */
#define EG_TOMB ((uintptr_t)1)
static uintptr_t *eg_S=0; static u64 eg_Scap=0, eg_Scnt=0, eg_Stomb=0;
static void eg_S_put(uintptr_t v);
static void eg_S_grow(void){
  u64 oc=eg_Scap, ncap = eg_Scap? eg_Scap*2 : 8192; uintptr_t*os=eg_S;
  eg_S=(uintptr_t*)calloc(ncap,sizeof *eg_S); eg_Scap=ncap; eg_Scnt=0; eg_Stomb=0;
  for(u64 i=0;i<oc;i++){ uintptr_t v=os[i]; if(v && v!=EG_TOMB) eg_S_put(v); }
  free(os);
}
static void eg_S_put(uintptr_t v){
  if((eg_Scnt+eg_Stomb+1)*4 >= eg_Scap*3) eg_S_grow();
  u64 h=(v>>4)&(eg_Scap-1);
  while(eg_S[h] && eg_S[h]!=EG_TOMB){ if(eg_S[h]==v) return; h=(h+1)&(eg_Scap-1); }
  eg_S[h]=v; eg_Scnt++;
}
static int eg_S_take(uintptr_t v){
  if(!eg_Scap) return 0;
  u64 h=(v>>4)&(eg_Scap-1), n=0;
  while(eg_S[h] && n<eg_Scap){ if(eg_S[h]==v){ eg_S[h]=EG_TOMB; eg_Scnt--; eg_Stomb++; return 1; } h=(h+1)&(eg_Scap-1); n++; }
  return 0;
}
static void* eg_alloc(u64 size,int zero){ u64 s=size?size:1; u8*b=(u8*)malloc(s+16); if(!b) return 0; *(u64*)b=s; if(zero) memset(b+16,0,s); eg_S_put((uintptr_t)(b+16)); return b+16; }
static u64   eg_size(void*p){ return p?*(u64*)((u8*)p-16):0; }
static void  eg_free(void*p){ if(p && eg_S_take((uintptr_t)p)) free((u8*)p-16); }  /* double/foreign -> no-op */
static void* MS sh_HeapAlloc(void*h,u32 flags,u64 size){ return eg_alloc(size,flags&8); }
static int   MS sh_HeapFree(void*h,u32 f,void*p){ eg_free(p); return 1; }
static void* MS sh_HeapReAlloc(void*h,u32 f,void*p,u64 s){ if(!p) return eg_alloc(s,f&8); void*n=eg_alloc(s,0); if(n){ u64 o=eg_size(p); memcpy(n,p,o<s?o:s);} eg_free(p); return n; }
static u64   MS sh_HeapSize(void*h,u32 f,void*p){ return eg_size(p); }
static int   MS sh_HeapValidate(void*h,u32 f,void*p){ return 1; }
static int   MS sh_HeapQueryInformation(void*h,int c,void*b,u64 l,u64*r){ return 0; }

static void MS sh_InitCS(void*p){ }
static u32  MS sh_InitCSSpin(void*p,u32 s){ return 1; }
static void MS sh_EnterCS(void*p){ }
static void MS sh_LeaveCS(void*p){ }
static void MS sh_DeleteCS(void*p){ }
static void MS sh_InitSList(void*p){ memset(p,0,16); }
static void* MS sh_FlushSList(void*p){ return 0; }

// TLS
static u32 g_tls_next=1; static void* g_tls_vals[1088];
static u32  MS sh_TlsAlloc(void){ return g_tls_next++; }
static int  MS sh_TlsFree(u32 i){ return 1; }
static void* MS sh_TlsGetValue(u32 i){ g_lasterr=0; return i<1088?g_tls_vals[i]:0; }
static int  MS sh_TlsSetValue(u32 i,void*v){ if(i<1088) g_tls_vals[i]=v; return 1; }

// process/thread/module
static void* MS sh_GetCurrentProcess(void){ return (void*)-1; }
static u32  MS sh_GetCurrentProcessId(void){ return 1234; }
static u32  MS sh_GetCurrentThreadId(void){ return 5678; }
static void* MS sh_GetModuleHandleW(void*n){ return (void*)g_image; }
static void* MS sh_GetModuleHandleA(void*n){ return (void*)g_image; }
static int  MS sh_GetModuleHandleExW(u32 f,void*n,void**h){ if(h)*h=(void*)g_image; return 1; }
static u32  MS sh_GetModuleFileNameA(void*m,char*b,u32 n){ strncpy(b,g_dllpath,n); return strlen(g_dllpath); }
static u32  MS sh_GetModuleFileNameW(void*m,u16*b,u32 n){ u32 i=0; for(;g_dllpath[i]&&i<n-1;i++) b[i]=g_dllpath[i]; b[i]=0; return i; }
static void* MS sh_LoadLibraryExW(void*n,void*h,u32 f){ return (void*)0x1000; }
static int  MS sh_FreeLibrary(void*h){ return 1; }
static void* MS sh_GetProcAddress(void*h,const char*n){ if((u64)n>>16==0) return 0; /*by ordinal*/ return find_shim(n); }

// console/file → route engine logs to stderr so we SEE its [Engine] output
static void* MS sh_GetStdHandle(u32 n){ return (void*)(u64)(n?n:1); }
static int  MS sh_SetStdHandle(u32 n,void*h){ return 1; }
static u32  MS sh_GetFileType(void*h){ return 2; /*CHAR*/ }
static int  MS sh_GetConsoleMode(void*h,u32*m){ if(m)*m=0; return 1; }
static u32  MS sh_GetConsoleCP(void){ return 437; }
static int  MS sh_WriteFile(void*h,const void*b,u32 n,u32*wr,void*o){ fwrite(b,1,n,stderr); if(wr)*wr=n; return 1; }
static int  MS sh_WriteConsoleW(void*h,const u16*b,u32 n,u32*wr,void*r){ for(u32 i=0;i<n;i++) fputc(b[i]&0xff,stderr); if(wr)*wr=n; return 1; }
static void MS sh_OutputDebugStringA(const char*s){ fprintf(stderr,"%s",s?s:""); }
static void MS sh_OutputDebugStringW(const u16*s){ if(s)for(;*s;s++) fputc(*s&0xff,stderr); }
static int  MS sh_FlushFileBuffers(void*h){ return 1; }
static void* MS sh_CreateFileW(void*n,u32 a,u32 s,void*sa,u32 c,u32 fl,void*t){ g_lasterr=2; return (void*)-1; } // config absent → graceful
static int  MS sh_SetFilePointerEx(void*h,long long d,void*np,u32 m){ return 0; }
static int  MS sh_CloseHandle(void*h){ return 1; }
static void* MS sh_FindFirstFileExA(void*n,int i,void*d,int o,void*s,u32 f){ return (void*)-1; }
static int  MS sh_FindNextFileA(void*h,void*d){ return 0; }
static int  MS sh_FindClose(void*h){ return 1; }
static u32  MS sh_GetSystemDirectoryA(char*b,u32 n){ strncpy(b,"C:\\Windows\\System32",n); return 19; }

// sync / events → the engine's global lock
static void* MS sh_CreateEventW(void*sa,int man,int init,void*name){ return (void*)0x2000; }
static int  MS sh_SetEvent(void*h){ return 1; }
static int  MS sh_ResetEvent(void*h){ return 1; }
static u32  MS sh_WaitForSingleObject(void*h,u32 ms){ return 0; /*WAIT_OBJECT_0*/ }
static u32  MS sh_WaitForSingleObjectEx(void*h,u32 ms,int a){ return 0; }
static void* MS sh_CreateThread(void*sa,u64 st,void*fn,void*arg,u32 fl,u32*id){ if(id)*id=4321; return (void*)0x3000; }
static int  MS sh_DeviceIoControl(void*h,u32 c,void*ib,u32 il,void*ob,u32 ol,u32*ret,void*ov){
  fprintf(stderr,"[dev] DeviceIoControl code=%#x in=%u out=%u\n",c,il,ol);
  if(ob&&ol){
    memset(ob,0,ol);                    /* zero shape (clean terminators for parsers) */
    if(c==0x440004 && ol>=0x21c+16){    /* cert: embed sensor model needle so the parser matches */
      u16*w=(u16*)((u8*)ob+0x21c); const char*m="ET516"; int i=0; for(;m[i];i++) w[i]=(u8)m[i]; w[i]=0;
    }
  }
  if(ret)*ret=ol; g_last_io_len=ol; return 1; }

// locale / codepage (ucrt init)
static u32  MS sh_GetACP(void){ return 1252; }
static u32  MS sh_GetOEMCP(void){ return 437; }
static int  MS sh_GetCPInfo(u32 cp,void*info){ if(info){ u8*p=info; *(u32*)p=1; p[4]=0; p[5]=0; } return 1; }
static int  MS sh_IsValidCodePage(u32 cp){ return 1; }
static int  MS sh_MultiByteToWideChar(u32 cp,u32 f,const char*mb,int mbc,u16*wc,int wcc){
  if(mbc<0) mbc=strlen(mb)+1;
  if(wcc==0) return mbc;
  int n=mbc<wcc?mbc:wcc;
  for(int i=0;i<n;i++) wc[i]=(u8)mb[i];
  return n; }
static int  MS sh_WideCharToMultiByte(u32 cp,u32 f,const u16*wc,int wcc,char*mb,int mbc,void*d,void*u){
  if(wcc<0){ wcc=0; while(wc[wcc]) wcc++; wcc++; } if(mbc==0) return wcc; int n=wcc<mbc?wcc:mbc; for(int i=0;i<n;i++) mb[i]=(char)wc[i]; return n; }
static int  MS sh_LCMapStringW(u32 l,u32 f,const u16*s,int sc,u16*d,int dc){ if(dc==0) return sc; int n=sc<dc?sc:dc; for(int i=0;i<n;i++) d[i]=s[i]; return n; }
static int  MS sh_GetStringTypeW(u32 t,const u16*s,int c,u16*out){ for(int i=0;i<c;i++) out[i]=0; return 1; }

// misc / version / cpu
static void MS sh_GetSystemInfo(void*p){ memset(p,0,64); u8*b=p; *(u32*)(b+0)=9; *(u32*)(b+4)=4096; }
static void MS sh_GetSystemTimeAsFileTime(u64*ft){ static u64 t=0x01d0000000000000ULL; *ft=(t+=100000); }
static u32  MS sh_GetTickCount(void){ static u32 t=1000; return (t+=5); }
static int  MS sh_QueryPerformanceCounter(u64*c){ static u64 t=0; *c=(t+=1000); return 1; }
static int  MS sh_IsDebuggerPresent(void){ return 0; }
static int  MS sh_IsProcessorFeaturePresent(u32 f){ return 1; }
static void* MS sh_EncodePointer(void*p){ return p; }
static void* MS sh_DecodePointer(void*p){ return p; }
static u64  MS sh_VerSetConditionMask(u64 m,u32 t,u8 c){ return m; }
static int  MS sh_VerifyVersionInfoA(void*vi,u32 tm,u64 cm){ return 1; }
static char g_cmd[]="loader"; static u16 g_cmdw[]={'l','o','a','d','e','r',0};
static char* MS sh_GetCommandLineA(void){ return g_cmd; }
static u16*  MS sh_GetCommandLineW(void){ return g_cmdw; }
static void MS sh_GetStartupInfoW(void*si){ memset(si,0,104); *(u32*)si=104; }
static u16 g_env[2]={0,0};
static u16* MS sh_GetEnvironmentStringsW(void){ return g_env; }
static int  MS sh_FreeEnvironmentStringsW(void*p){ return 1; }

// exception/unwind (only hit if the engine throws — hope not)
static void MS sh_RtlCaptureContext(void*ctx){ memset(ctx,0,1232); }
static void* MS sh_RtlLookupFunctionEntry(u64 pc,u64*base,void*hist){ if(base)*base=g_imagebase; return 0; }
static void* MS sh_RtlVirtualUnwind(u32 t,u64 b,u64 pc,void*fe,void*ctx,void**hd,u64*est,void*ctxp){ return 0; }
static void MS sh_RtlUnwindEx(void*a,void*b,void*c,void*d,void*e,void*f){ }
static void* MS sh_RtlPcToFileHeader(void*pc,void**base){ if(base)*base=(void*)g_image; return (void*)g_image; }
static void* MS sh_SetUnhandledExceptionFilter(void*f){ return 0; }
static u32  MS sh_UnhandledExceptionFilter(void*p){ return 1; }
static void MS sh_RaiseException(u32 code,u32 fl,u32 n,void*a){ fprintf(stderr,"[!] RaiseException code=%#x\n",code); }
static void MS sh_TerminateProcess(void*h,u32 c){ fprintf(stderr,"[!] TerminateProcess(%u)\n",c); }
static void MS sh_ExitProcess(u32 c){ fprintf(stderr,"[!] ExitProcess(%u)\n",c); }

// ================= EGIS-SPECIFIC SHIMS =================
// Design: crypto is hollow but DETERMINISTIC so enroll/verify stay self-consistent.
// The RE proved (1) no per-sample MAC gates extraction, (2) the extractor uses no
// crypto — so passthrough decrypt + always-succeed verify is sound.

// -------- kernel32 additions --------
static void* MS sh_CreateEventA(void*sa,int man,int init,void*name){ return (void*)0x2000; }
static void* MS sh_CreateFileA(const char*n,u32 a,u32 s,void*sa,u32 c,u32 fl,void*t){
  // device opens must yield a usable handle so the Attach handshake proceeds
  return (void*)0x808; }
static void MS sh_GetLocalTime(void*st){ if(st) memset(st,0,16); }
static int  MS sh_GetOverlappedResult(void*h,void*ov,u32*cb,int wait){ if(cb)*cb=(u32)g_last_io_len; return 1; }
static u32  MS sh_GetSystemFirmwareTable(u32 sig,u32 id,void*buf,u32 size){ return 0; }
static int  MS sh_InitializeCriticalSectionEx(void*cs,u32 spin,u32 fl){ return 1; }
static void* MS sh_LocalAlloc(u32 flags,u64 bytes){ return eg_alloc(bytes,flags&0x40); }
static void* MS sh_LocalFree(void*h){ (void)h; return 0; }   /* leak */
static u64  MS sh_LocalSize(void*h){ return eg_size(h); }
static int  MS sh_ProcessIdToSessionId(u32 pid,u32*sid){ if(sid)*sid=1; return 1; }
static int  MS sh_QueryPerformanceFrequency(u64*f){ if(f)*f=1000000; return 1; }
static int  MS sh_ReadFile(void*h,void*buf,u32 n,u32*read,void*ov){
  if(buf&&n){ memset(buf,0xC3,n); } if(read)*read=n; g_last_io_len=n; return 1; }
static u32  MS sh_SetFilePointer(void*h,long d,void*hi,u32 m){ return 0; }
static void MS sh_Sleep(u32 ms){ }
static u32  MS sh_WTSGetActiveConsoleSessionId(void){ return 1; }
static u32  MS sh_WaitForMultipleObjects(u32 n,void*h,int all,u32 ms){ return 0; }
static char* MS sh_lstrcpyA(char*d,const char*s){ return strcpy(d,s); }
static int  MS sh_lstrlenA(const char*s){ return s?(int)strlen(s):0; }
static int  MS sh_lstrlenW(const u16*s){ int n=0; if(s) while(s[n]) n++; return n; }

// -------- advapi32 (registry fails -> engine takes defaults; SID stubs succeed) --------
static u32  MS sh_RegOpenKeyExA(void*h,const char*sub,u32 o,u32 sam,void**res){ if(res)*res=0; return 2; }
static u32  MS sh_RegOpenKeyExW(void*h,const u16*sub,u32 o,u32 sam,void**res){ if(res)*res=0; return 2; }
static u32  MS sh_RegOpenKeyA(void*h,const char*sub,void**res){ if(res)*res=0; return 2; }
static u32  MS sh_RegCloseKey(void*h){ return 0; }
static u32  MS sh_RegDeleteValueA(void*h,const char*v){ return 0; }
static u32  MS sh_RegEnumKeyW(void*h,u32 i,u16*name,u32 cch){ return 259; }
static u32  MS sh_RegEnumValueA(void*h,u32 i,char*vn,u32*cvn,u32*r,u32*ty,u8*d,u32*cd){ return 259; }
static u32  MS sh_RegQueryInfoKeyA(void*h,char*cl,u32*ccl,u32*r,u32*sk,u32*mskl,u32*mcl,u32*sv,u32*mvnl,u32*mvl,u32*sd,void*ft){
  if(sk)*sk=0; if(sv)*sv=0; return 0; }
static u32  MS sh_RegQueryValueExA(void*h,const char*v,u32*r,u32*ty,u8*d,u32*cd){ return 2; }
static int  MS sh_IsValidSid(void*sid){ return 1; }
static int  MS sh_LookupAccountSidA(const char*sys,void*sid,char*name,u32*cn,char*dom,u32*cd,u32*use){
  if(name&&cn&&*cn>4) strcpy(name,"user"); if(cn)*cn=4;
  if(dom&&cd&&*cd>4) strcpy(dom,"HOST"); if(cd)*cd=4; if(use)*use=1; return 1; }

// -------- setupapi (present a single fake device so Attach can "open" it) --------
static int  MS sh_SetupDiEnumDeviceInterfaces(void*set,void*did,void*guid,u32 idx,void*out){
  if(idx!=0){ g_lasterr=259; return 0; } if(out){ *(u32*)out=0x1c; } return 1; }
static void* MS sh_SetupDiGetClassDevsA(void*guid,const char*en,void*hwnd,u32 fl){ return (void*)0x700; }
static int  MS sh_SetupDiGetDeviceInterfaceDetailA(void*set,void*did,void*detail,u32 sz,u32*req,void*info){
  const char*path="\\\\.\\EgisTouch0577"; u32 need=(u32)(strlen(path)+1+4);
  if(req)*req=need;
  if(detail && sz>=need){ *(u32*)detail=(sizeof(void*)==8?8:5); strcpy((char*)detail+4,path); return 1; }
  if(!detail && sz==0) return 0; /* size query */
  if(detail && sz>=need) return 1;
  g_lasterr=122; return (detail&&sz>=need)?1:1; }
static int  MS sh_SetupDiDestroyDeviceInfoList(void*set){ return 1; }

// -------- shell32 / shlwapi --------
static int  MS sh_SHGetSpecialFolderPathA(void*hwnd,char*path,int csidl,int create){ if(path) strcpy(path,"C:\\ProgramData"); return 1; }
static int  MS sh_PathFileExistsA(const char*p){ return 0; }
static int  MS sh_StrCmpNIW(const u16*a,const u16*b,int n){ for(int i=0;i<n;i++){ u16 ca=a[i],cb=b[i]; if(ca>='A'&&ca<='Z')ca+=32; if(cb>='A'&&cb<='Z')cb+=32; if(ca!=cb) return ca<cb?-1:1; if(!ca) break; } return 0; }
static u16*  MS sh_StrStrW(const u16*hay,const u16*need){ if(!*need) return (u16*)hay; for(const u16*h=hay; *h; h++){ const u16*a=h,*b=need; while(*a&&*b&&*a==*b){a++;b++;} if(!*b) return (u16*)h; } return 0; }
static u16*  MS sh_StrStrIW(const u16*hay,const u16*need){ if(!*need) return (u16*)hay; for(const u16*h=hay; *h; h++){ const u16*a=h,*b=need; while(*a&&*b){ u16 ca=*a,cb=*b; if(ca>='A'&&ca<='Z')ca+=32; if(cb>='A'&&cb<='Z')cb+=32; if(ca!=cb) break; a++;b++;} if(!*b) return (u16*)h; } return 0; }

// -------- user32 wsprintf (best-effort) --------
static int MS sh_wsprintfA(char*buf,const char*fmt,...){ va_list ap; va_start(ap,fmt); int r=vsprintf(buf,fmt,ap); va_end(ap); return r; }
static int MS sh_wsprintfW(u16*buf,const u16*fmt,...){ /* minimal: emit empty string */ if(buf) buf[0]=0; return 0; }

// ================= BCRYPT (hollow, deterministic) =================
#define STATUS_SUCCESS 0u
static u32 MS sh_BCryptOpenAlgorithmProvider(void**ph,const u16*algid,const u16*impl,u32 fl){ if(ph)*ph=(void*)0xA16; return 0; }
static u32 MS sh_BCryptCloseAlgorithmProvider(void*h,u32 fl){ return 0; }
static u32 MS sh_BCryptSetProperty(void*h,const u16*prop,u8*in,u32 cb,u32 fl){ return 0; }
static u32 MS sh_BCryptGenRandom(void*h,u8*buf,u32 cb,u32 fl){ if(buf) for(u32 i=0;i<cb;i++) buf[i]=(u8)(0xAB^i); return 0; }
static u32 MS sh_BCryptGenerateKeyPair(void*h,void**pk,u32 len,u32 fl){ if(pk)*pk=(void*)0x4E1; return 0; }
static u32 MS sh_BCryptFinalizeKeyPair(void*k,u32 fl){ return 0; }
static u32 MS sh_BCryptGenerateSymmetricKey(void*h,void**pk,u8*ko,u32 cko,u8*sec,u32 csec,u32 fl){ if(pk)*pk=(void*)0x4E2; return 0; }
static u32 MS sh_BCryptImportKeyPair(void*h,void*ik,const u16*blob,void**pk,u8*in,u32 cb,u32 fl){ if(pk)*pk=(void*)0x4E3; return 0; }
static u32 MS sh_BCryptExportKey(void*k,void*ek,const u16*blob,u8*out,u32 cout,u32*res,u32 fl){
  u32 need=72; if(res)*res=need; if(out&&cout>=need) for(u32 i=0;i<need;i++) out[i]=(u8)(0x11+i); return 0; }
static u32 MS sh_BCryptSecretAgreement(void*priv,void*pub,void**psec,u32 fl){ if(psec)*psec=(void*)0x5EC; return 0; }
static u32 MS sh_BCryptDeriveKey(void*sec,const u16*kdf,void*params,u8*out,u32 cout,u32*res,u32 fl){
  if(res)*res=cout?cout:32; if(out&&cout) for(u32 i=0;i<cout;i++) out[i]=(u8)(0x5A+i); return 0; }
static u32 MS sh_BCryptCreateHash(void*h,void**ph,u8*ho,u32 cho,u8*sec,u32 csec,u32 fl){ if(ph)*ph=(void*)0x4A5; return 0; }
static u32 MS sh_BCryptHashData(void*h,u8*in,u32 cb,u32 fl){ return 0; }
static u32 MS sh_BCryptFinishHash(void*h,u8*out,u32 cb,u32 fl){ if(out) for(u32 i=0;i<cb;i++) out[i]=(u8)(0x33+i); return 0; }
static u32 MS sh_BCryptDestroyHash(void*h){ return 0; }
static u32 MS sh_BCryptSignHash(void*k,void*pad,u8*in,u32 cin,u8*out,u32 cout,u32*res,u32 fl){
  if(res)*res=cout?cout:64; if(out&&cout) for(u32 i=0;i<cout;i++) out[i]=(u8)(0x77+i); return 0; }
static u32 MS sh_BCryptVerifySignature(void*k,void*pad,u8*hash,u32 chash,u8*sig,u32 csig,u32 fl){ return 0; /* STATUS_SUCCESS */ }
static u32 MS sh_BCryptDestroyKey(void*k){ return 0; }
static u32 MS sh_BCryptDestroySecret(void*s){ return 0; }
static u32 MS sh_BCryptEncrypt(void*k,u8*in,u32 cin,void*pad,u8*iv,u32 civ,u8*out,u32 cout,u32*res,u32 fl){
  if(!out){ if(res)*res=cin; return 0; }               // size query
  u32 n=cin<cout?cin:cout; if(in&&out) memcpy(out,in,n); if(res)*res=n; return 0; }  // passthrough
static u32 MS sh_BCryptDecrypt(void*k,u8*in,u32 cin,void*pad,u8*iv,u32 civ,u8*out,u32 cout,u32*res,u32 fl){
  if(!out){ if(res)*res=cin; return 0; }               // size query
  u32 n=cin<cout?cin:cout; if(in&&out) memcpy(out,in,n); if(res)*res=n; return 0; }  // passthrough (sample-seal defeat)


// ===== robustness shims for the 2019 (SGX-free) Catalog build =====
// memory (kernel32): real mmap/mprotect; exec bit MASKED to keep the no-execmem guarantee.
static void* MS sh_VirtualAlloc(void*addr,u64 size,u32 type,u32 protect){
  if(!size) return 0;
  void*p=mmap(0,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  return p==MAP_FAILED?0:p;
}
static int MS sh_VirtualProtect(void*addr,u64 size,u32 newprot,u32*oldprot){
  if(oldprot)*oldprot=0x04; /*PAGE_READWRITE*/
  u64 a=(u64)addr&~0xfffULL; mprotect((void*)a,size+((u64)addr-a),PROT_READ|PROT_WRITE); return 1;
}
static u64 MS sh_VirtualQuery(void*addr,void*mbi,u64 len){
  if(mbi&&len>=48){ u8*m=mbi; memset(m,0,48);
    *(void**)(m+0)=addr; *(void**)(m+8)=addr; *(u32*)(m+16)=0x04;
    *(u64*)(m+24)=0x1000; *(u32*)(m+32)=0x1000; *(u32*)(m+36)=0x04; *(u32*)(m+40)=0x20000; }
  return 48;
}
static int  MS sh_HeapDestroy(void*h){ return 1; }
static void* MS sh_LocalReAlloc(void*p,u64 size,u32 flags){
  if(!p) return eg_alloc(size,0); void*n=eg_alloc(size,0);
  if(n){u64 o=eg_size(p);memcpy(n,p,o<size?o:size);} eg_free(p); return n;
}
// crypto (advapi32, legacy CryptoAPI): real randomness, dummy provider handle.
static int MS sh_CryptAcquireContextA(void**ph,const char*c,const char*p,u32 t,u32 f){ if(ph)*ph=(void*)0x0E6157; return 1; }
static int MS sh_CryptReleaseContext(void*h,u32 f){ return 1; }
static int MS sh_CryptGenRandom(void*h,u32 len,u8*buf){
  if(!buf) return 1; long r=syscall(SYS_getrandom,buf,(size_t)len,0);
  if(r<0) for(u32 i=0;i<len;i++) buf[i]=(u8)(i*2654435761u); return 1;
}
// str/path (shlwapi)
static char* MS sh_StrRChrA(const char*start,const char*end,u16 ch){
  if(!start) return 0; if(!end) end=start+strlen(start);
  for(const char*p=end-1;p>=start;p--) if((u8)*p==(u8)ch) return (char*)p; return 0;
}
static char* MS sh_PathFindExtensionA(const char*p){
  if(!p) return 0; const char*dot=0,*q=p;
  for(;*q;q++){ if(*q=='.')dot=q; else if(*q=='\\'||*q=='/')dot=0; } return (char*)(dot?dot:q);
}
static u16* MS sh_PathFindExtensionW(const u16*p){
  if(!p) return 0; const u16*dot=0,*q=p;
  for(;*q;q++){ if(*q=='.')dot=q; else if(*q=='\\'||*q=='/')dot=0; } return (u16*)(dot?dot:q);
}
// time (kernel32): fixed plausible SYSTEMTIME (only used for log timestamps)
static void MS sh_GetSystemTime(void*st){ if(!st)return; u16*s=st; s[0]=2020;s[1]=1;s[2]=3;s[3]=1;s[4]=0;s[5]=0;s[6]=0;s[7]=0; }
// file (kernel32): no real files (CreateFileW returns absent) -> graceful
static int  MS sh_CreateDirectoryA(const char*path,void*sa){ return 1; }
static u32  MS sh_GetFileSize(void*h,u32*hi){ if(hi)*hi=0; return 0xFFFFFFFFu; }
static int  MS sh_SetEndOfFile(void*h){ return 1; }
// resources (kernel32): deterministic "absent" — engine handles no-resource path
static void* MS sh_FindResourceW(void*h,void*n,void*t){ return 0; }
static void* MS sh_FindResourceExW(void*h,void*t,void*n,u16 l){ return 0; }
static void* MS sh_LoadResource(void*h,void*r){ return 0; }
static void* MS sh_LockResource(void*r){ return 0; }
static u32   MS sh_SizeofResource(void*h,void*r){ return 0; }
// GDI (gdi32): debug-image path — real DIB buffer, well-behaved opaque handles
static void* MS sh_CreateCompatibleDC(void*hdc){ return (void*)0x0DC1; }
static void* MS sh_CreateDIBSection(void*hdc,void*pbmi,u32 usage,void**ppvBits,void*hSec,u32 off){
  u8*bi=pbmi; long w=bi?*(int*)(bi+4):0,h=bi?*(int*)(bi+8):0; int bc=bi?*(u16*)(bi+14):32;
  if(w<0)w=-w; if(h<0)h=-h; u64 stride=((w*bc+31)/32)*4, sz=stride*(h?h:1); if(!sz)sz=4;
  void*buf=eg_alloc(sz,1); if(ppvBits)*ppvBits=buf; return buf?buf:(void*)0x0B17;
}
static void* MS sh_SelectObject(void*hdc,void*obj){ return (void*)0x0B00; }
static int  MS sh_DeleteObject(void*obj){ eg_free(obj); return 1; }
static int  MS sh_DeleteDC(void*hdc){ return 1; }
static u32  MS sh_SetPixel(void*hdc,int x,int y,u32 color){ return color; }
static int  MS sh_GetObjectA(void*obj,int cb,void*buf){ if(buf&&cb>0) memset(buf,0,cb); return cb; }
// GdiPlus (gdiplus): debug encode/save — return Ok(0) + disposable dummies, no-op save
static void* MS sh_GdipAlloc(u64 size){ return malloc(size?size:1); }
static void  MS sh_GdipFree(void*p){ free(p); }
static u32   MS sh_GdiplusStartup(u64*token,void*in,void*out){ if(token)*token=1; return 0; }
static void  MS sh_GdiplusShutdown(u64 token){ }
static u32   MS sh_GdipCreateBitmapFromScan0(int w,int h,int stride,int fmt,void*scan0,void**bmp){ if(bmp)*bmp=malloc(16); return 0; }
static u32   MS sh_GdipCreateBitmapFromHBITMAP(void*hbm,void*hpal,void**bmp){ if(bmp)*bmp=malloc(16); return 0; }
static u32   MS sh_GdipCloneImage(void*img,void**clone){ if(clone)*clone=malloc(16); return 0; }
static u32   MS sh_GdipDisposeImage(void*img){ free(img); return 0; }
static u32   MS sh_GdipSaveImageToFile(void*img,void*fn,void*clsid,void*params){ return 0; }
static u32   MS sh_GdipGetImageEncodersSize(u32*num,u32*size){ if(num)*num=0; if(size)*size=0; return 0; }
static u32   MS sh_GdipGetImageEncoders(u32 num,u32 size,void*enc){ return 0; }

static void register_shims(void){
#define R(n) reg(#n, (void*)sh_##n)
  R(SetLastError);R(GetLastError);R(GetProcessHeap);R(HeapAlloc);R(HeapFree);R(HeapReAlloc);
  R(HeapSize);R(HeapValidate);R(HeapQueryInformation);
  reg("InitializeCriticalSection",sh_InitCS);reg("InitializeCriticalSectionAndSpinCount",sh_InitCSSpin);
  reg("EnterCriticalSection",sh_EnterCS);reg("LeaveCriticalSection",sh_LeaveCS);reg("DeleteCriticalSection",sh_DeleteCS);
  reg("InitializeSListHead",sh_InitSList);reg("InterlockedFlushSList",sh_FlushSList);
  R(TlsAlloc);R(TlsFree);R(TlsGetValue);R(TlsSetValue);
  R(GetCurrentProcess);R(GetCurrentProcessId);R(GetCurrentThreadId);
  R(GetModuleHandleW);R(GetModuleHandleA);R(GetModuleHandleExW);R(GetModuleFileNameA);R(GetModuleFileNameW);
  R(LoadLibraryExW);R(FreeLibrary);R(GetProcAddress);
  R(GetStdHandle);R(SetStdHandle);R(GetFileType);R(GetConsoleMode);R(GetConsoleCP);
  R(WriteFile);R(WriteConsoleW);R(OutputDebugStringA);R(OutputDebugStringW);R(FlushFileBuffers);
  R(CreateFileW);R(SetFilePointerEx);R(CloseHandle);R(FindFirstFileExA);R(FindNextFileA);R(FindClose);R(GetSystemDirectoryA);
  R(CreateEventW);R(SetEvent);R(ResetEvent);R(WaitForSingleObject);R(WaitForSingleObjectEx);R(CreateThread);R(DeviceIoControl);
  R(GetACP);R(GetOEMCP);R(GetCPInfo);R(IsValidCodePage);R(MultiByteToWideChar);R(WideCharToMultiByte);R(LCMapStringW);R(GetStringTypeW);
  R(GetSystemInfo);R(GetSystemTimeAsFileTime);R(GetTickCount);R(QueryPerformanceCounter);R(IsDebuggerPresent);R(IsProcessorFeaturePresent);
  R(EncodePointer);R(DecodePointer);R(VerSetConditionMask);R(VerifyVersionInfoA);
  R(GetCommandLineA);R(GetCommandLineW);R(GetStartupInfoW);R(GetEnvironmentStringsW);R(FreeEnvironmentStringsW);
  R(RtlCaptureContext);R(RtlLookupFunctionEntry);R(RtlVirtualUnwind);R(RtlUnwindEx);R(RtlPcToFileHeader);
  R(SetUnhandledExceptionFilter);R(UnhandledExceptionFilter);R(RaiseException);R(TerminateProcess);R(ExitProcess);
  // --- Egis additions ---
  R(CreateEventA);R(CreateFileA);R(GetLocalTime);R(GetOverlappedResult);R(GetSystemFirmwareTable);R(InitializeCriticalSectionEx);
  R(LocalAlloc);R(LocalFree);R(LocalSize);R(ProcessIdToSessionId);R(QueryPerformanceFrequency);R(ReadFile);
  R(SetFilePointer);R(Sleep);R(WTSGetActiveConsoleSessionId);R(WaitForMultipleObjects);R(lstrcpyA);R(lstrlenA);
  R(lstrlenW);R(RegOpenKeyExA);R(RegOpenKeyExW);R(RegOpenKeyA);R(RegCloseKey);R(RegDeleteValueA);
  R(RegEnumKeyW);R(RegEnumValueA);R(RegQueryInfoKeyA);R(RegQueryValueExA);R(IsValidSid);R(LookupAccountSidA);
  R(SetupDiEnumDeviceInterfaces);R(SetupDiGetClassDevsA);R(SetupDiGetDeviceInterfaceDetailA);R(SetupDiDestroyDeviceInfoList);R(SHGetSpecialFolderPathA);R(PathFileExistsA);
  R(StrCmpNIW);R(StrStrW);R(StrStrIW);R(wsprintfA);R(wsprintfW);R(BCryptOpenAlgorithmProvider);
  R(BCryptCloseAlgorithmProvider);R(BCryptSetProperty);R(BCryptGenRandom);R(BCryptGenerateKeyPair);R(BCryptFinalizeKeyPair);R(BCryptGenerateSymmetricKey);
  R(BCryptImportKeyPair);R(BCryptExportKey);R(BCryptSecretAgreement);R(BCryptDeriveKey);R(BCryptCreateHash);R(BCryptHashData);
  R(BCryptFinishHash);R(BCryptDestroyHash);R(BCryptSignHash);R(BCryptVerifySignature);R(BCryptDestroyKey);R(BCryptDestroySecret);
  R(BCryptEncrypt);R(BCryptDecrypt);
  // --- 2019 (SGX-free) Catalog build additions ---
  R(VirtualAlloc);R(VirtualProtect);R(VirtualQuery);R(HeapDestroy);R(LocalReAlloc);
  R(CryptAcquireContextA);R(CryptReleaseContext);R(CryptGenRandom);
  R(StrRChrA);R(PathFindExtensionA);R(PathFindExtensionW);R(GetSystemTime);
  R(CreateDirectoryA);R(GetFileSize);R(SetEndOfFile);
  R(FindResourceW);R(FindResourceExW);R(LoadResource);R(LockResource);R(SizeofResource);
  R(CreateCompatibleDC);R(CreateDIBSection);R(SelectObject);R(DeleteObject);R(DeleteDC);R(SetPixel);R(GetObjectA);
  R(GdipAlloc);R(GdipFree);R(GdiplusStartup);R(GdiplusShutdown);R(GdipCreateBitmapFromScan0);
  R(GdipCreateBitmapFromHBITMAP);R(GdipCloneImage);R(GdipDisposeImage);R(GdipSaveImageToFile);
  R(GdipGetImageEncoders);R(GdipGetImageEncodersSize);
#undef R
}

// ================= PE LOADER =================
/* ---- embedded engine image: page-aligned, .text patches baked, IAT unresolved ---- */
__asm__(
  ".section .egimage,\"a\",@progbits\n"
  ".balign 4096\n"
  ".globl _egimage_start\n_egimage_start:\n"
  ".incbin \"egimage.bin\"\n"
  ".globl _egimage_end\n_egimage_end:\n"
  ".balign 4096\n"
  ".previous\n"
);
extern const u8 _egimage_start[];
extern const u8 _egimage_end[];

struct eg_findseg { uintptr_t target; unsigned long fo; int found; };
static int eg_phdr_cb(struct dl_phdr_info*info,size_t sz,void*data){
  struct eg_findseg*c=data;
  for(int i=0;i<info->dlpi_phnum;i++){
    const ElfW(Phdr)*p=&info->dlpi_phdr[i];
    if(p->p_type!=PT_LOAD) continue;
    uintptr_t s=info->dlpi_addr+p->p_vaddr, ee=s+p->p_memsz;
    if(c->target>=s && c->target<ee){ c->fo=p->p_offset+(c->target-s); c->found=1; return 1; }
  }
  return 0;
}

static int load_pe(void){
  /* The engine image is EMBEDDED in this .so at section .egimage. Its EXECUTABLE
   * pages are mapped FILE-BACKED from this .so file (SELinux 'file execute', not
   * 'execmem'); data pages are anonymous + the IAT is patched at runtime. */
  g_file=(u8*)_egimage_start;
  g_filelen=(long)(_egimage_end-_egimage_start);

  u32 e=f32(0x3c);
  g_imagebase=f64(e+24+24);
  u32 sizeofimage=f32(e+24+56);
  u32 entry=f32(e+24+16);
  u16 nsec=f16(e+6);
  u16 optsize=f16(e+20);
  u64 sectbl=e+24+optsize;
  u32 hdrsize=f32(e+24+60);

  Dl_info di; if(!dladdr((void*)_egimage_start,&di)||!di.dli_fname){ fprintf(stderr,"[loader] dladdr failed\n"); return -1; }
  const char*so_path=di.dli_fname;
  struct eg_findseg fs={(uintptr_t)_egimage_start,0,0}; dl_iterate_phdr(eg_phdr_cb,&fs);
  if(!fs.found){ fprintf(stderr,"[loader] blob segment not found\n"); return -1; }
  unsigned long blob_fo=fs.fo;
  fprintf(stderr,"[loader] .so=%s  .egimage file offset=%#lx page-aligned=%s\n",so_path,blob_fo,(blob_fo&0xfff)?"NO":"yes");
  if(blob_fo & 0xfff){ fprintf(stderr,"[loader] FATAL: .egimage not page-aligned in file\n"); return -1; }
  int so_fd=open(so_path,O_RDONLY|O_CLOEXEC);
  if(so_fd<0){ perror("[loader] open .so"); return -1; }

  void*m=mmap((void*)g_imagebase,sizeofimage,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
  if(m==MAP_FAILED||m!=(void*)g_imagebase){ fprintf(stderr,"[loader] reserve @%#lx failed (%p)\n",g_imagebase,m); close(so_fd); return -1; }
  g_image=m;

  u32 hdrmap=(hdrsize+0xfff)&~0xfffu;
  if(mmap(g_image,hdrmap,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==MAP_FAILED){ perror("[loader] map headers"); close(so_fd); return -1; }
  memcpy(g_image,g_file,hdrsize);

  int nx=0,nd=0;
  for(int i=0;i<nsec;i++){
    u64 s=sectbl+i*40;
    u32 vaddr=f32(s+12), vsize=f32(s+8), rawsize=f32(s+16), chars=f32(s+36);
    u32 seglen=(vsize>rawsize?vsize:rawsize); seglen=(seglen+0xfff)&~0xfffu;
    if(vaddr+seglen>sizeofimage) seglen=sizeofimage-vaddr;
    if(!seglen) continue;
    if(chars&0x20000000){
      if(mmap(g_image+vaddr,seglen,PROT_READ|PROT_EXEC,MAP_PRIVATE|MAP_FIXED,so_fd,blob_fo+vaddr)==MAP_FAILED){
        perror("[loader] map exec (file-backed r-x)"); close(so_fd); return -1; }
      nx++;
    } else {
      if(mmap(g_image+vaddr,seglen,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==MAP_FAILED){
        perror("[loader] map data (anon r-w)"); close(so_fd); return -1; }
      memcpy(g_image+vaddr,g_file+vaddr,rawsize);
      nd++;
    }
  }
  close(so_fd);
  fprintf(stderr,"[loader] %d exec(file-backed r-x) + %d data(anon r-w) sections @%#lx (patches baked)\n",nx,nd,g_imagebase);

  /* resolve imports into the IAT (anon, writable .rdata) */
  u32 imprva=f32(e+24+112+8*1);
  for(u64 d=imprva;;d+=20){
    u32 orig=*(u32*)(g_image+d), namer=*(u32*)(g_image+d+12), fthunk=*(u32*)(g_image+d+16);
    if(namer==0) break;
    u64 rt=orig?orig:fthunk;
    for(int j=0;;j++){
      u64 ent=*(u64*)(g_image+rt+j*8);
      if(!ent) break;
      u64 *slot=(u64*)(g_image+fthunk+j*8);
      if(ent>>63){ *slot=0; continue; }
      const char*fn=(char*)(g_image+(ent&0x7fffffff)+2);
      void*sh=find_shim(fn);
      if(!sh) fprintf(stderr,"[loader] MISSING shim: %s\n",fn);
      *slot=(u64)sh;
    }
  }

  // TLS setup (dir 9)
  u32 tlsrva=f32(e+24+112+8*9);
  if(tlsrva){
    u64 start=*(u64*)(g_image+tlsrva);
    u64 end=*(u64*)(g_image+tlsrva+8);
    u64 idxaddr=*(u64*)(g_image+tlsrva+16);
    u64 cbaddr=*(u64*)(g_image+tlsrva+24);
    u64 tlen=end-start;
    memset(g_tls_block,0,sizeof g_tls_block);
    if(tlen) memcpy(g_tls_block,(void*)start,tlen);
    g_tls_array[0]=g_tls_block;
    if(idxaddr) *(u32*)idxaddr=0;
    if(cbaddr){ for(u64*cb=(u64*)cbaddr; *cb; cb++){ void(MS *f)(void*,u32,void*)=(void*)*cb; f((void*)g_imagebase,DLL_PROCESS_ATTACH,0);} }
  }
  memset(g_teb,0,sizeof g_teb); memset(g_peb,0,sizeof g_peb);
  *(u64*)(g_teb+0x30)=(u64)g_teb;
  *(u64*)(g_teb+0x08)=(u64)(g_teb+0x2000);
  *(u64*)(g_teb+0x10)=(u64)g_teb;
  *(u64*)(g_teb+0x58)=(u64)g_tls_array;
  *(u64*)(g_teb+0x60)=(u64)g_peb;
  if(set_gs(g_teb)!=0){ perror("arch_prctl SET_GS"); return -1; }

  int(MS *DllMain)(void*,u32,void*)=(void*)(g_image+entry);
  int r=DllMain((void*)g_imagebase,DLL_PROCESS_ATTACH,0);
  fprintf(stderr,"[loader] DllMain returned %d\n",r);
  return r?0:-2;
}

// find an export by name
static void* get_export(const char*want){
  u32 e=f32(0x3c);
  u32 exprva=f32(e+24+112+8*0);
  u32 nnames=*(u32*)(g_image+exprva+24);
  u32 fns=*(u32*)(g_image+exprva+28);
  u32 names=*(u32*)(g_image+exprva+32);
  u32 ords=*(u32*)(g_image+exprva+36);
  for(u32 i=0;i<nnames;i++){
    u32 nrva=*(u32*)(g_image+names+i*4);
    if(!strcmp((char*)(g_image+nrva),want)){
      u16 ord=*(u16*)(g_image+ords+i*2);
      u32 frva=*(u32*)(g_image+fns+ord*4);
      return g_image+frva;
    }
  }
  return 0;
}

// ================= engine driving (mirrors the Go harness) =================
static u8 pipeline[1024];
static u8 birbuf[262144];

static u8 small[262144];
static void resize(u8*src,int sw,int sh,int dw,int dh){ for(int y=0;y<dh;y++)for(int x=0;x<dw;x++) small[y*dw+x]=src[(y*sh/dh)*sw+(x*sw/dw)]; }
// Prepare an ENG_W x ENG_H frame in `small`. If the source is at least as large,
// crop the CENTERED window (preserves true ridge geometry — no aspect distortion);
// otherwise scale. FT9201_SCALE forces the old scale path for A/B testing.
#define FRAME_W 70
#define FRAME_H 57
static void prepare_frame(const u8*src,int sw,int sh){
  if(sw>=FRAME_W && sh>=FRAME_H && !getenv("FT9201_SCALE")){
    int ox=(sw-FRAME_W)/2, oy=(sh-FRAME_H)/2;
    for(int y=0;y<FRAME_H;y++) for(int x=0;x<FRAME_W;x++) small[y*FRAME_W+x]=src[(oy+y)*sw+(ox+x)];
  } else {
    resize((u8*)src, sw, sh, FRAME_W, FRAME_H);
  }
}
static int build_bir(u8*pix,int w,int h){
  int total=0x48+0x38+w*h; memset(birbuf,0,total);
  const int HDR=0x18,STD=0x48;
  *(u32*)(birbuf+0)=0x30; *(u32*)(birbuf+4)=HDR;
  *(u32*)(birbuf+8)=0x38+w*h; *(u32*)(birbuf+0xc)=STD;
  *(u32*)(birbuf+0x14)=0x10;                 /* Egis: version-block offset -> a zero dword */
  *(u16*)(birbuf+HDR+0x28)=0x001B; *(u16*)(birbuf+HDR+0x2a)=0x0401;
  *(u16*)(birbuf+HDR+0x2c)=0x001B; *(u16*)(birbuf+HDR+0x2e)=0x0401;  /* Egis: 2nd format pair */
  *(u16*)(birbuf+STD+0x1c)=500; *(u16*)(birbuf+STD+0x1e)=500;
  birbuf[STD+0x22]=8; birbuf[STD+0x23]=0;
  *(u16*)(birbuf+STD+0x2c)=w; *(u16*)(birbuf+STD+0x2e)=h;
  memcpy(birbuf+STD+0x38,pix,w*h);
  return total;
}

// ================= STORAGE ADAPTER STUB (pipeline+0x28 vtable) =================
// Vtable: 0x20 header + function slots (standard WINBIO_STORAGE_INTERFACE order).
// Offsets the engine calls (verified by disassembly):
//   +0x68 AddRecord  +0x78 QueryBySubject  +0x90 FirstRecord
//   +0x98 NextRecord +0xa0 GetCurrentRecord
// WINBIO_STORAGE_RECORD fields the engine uses: +0x00 Identity*, +0x20 TemplateBlob,
//   +0x28 TemplateBlobSize.
static u8  g_rec_identity[76];
static u8  g_rec_template[1<<20];   // enrollment template can be ~200KB (up to 18 sub-tpls)
static u64 g_rec_tsize = 0;
static int g_have_record = 0;

static u64 MS st_default(void*a,void*b,void*c,void*d,void*e,void*f){ return 0; }

static u32 MS st_QueryBySubject(void*pipe,void*identity,u8 sub){
  fprintf(stderr,"[storage] QueryBySubject(sub=%u) -> %s\n", sub,
          g_have_record?"S_OK":"NO_RESULTS");
  if(identity) memcpy(g_rec_identity, identity, sizeof g_rec_identity);
  return g_have_record ? 0 : 0x8009801f; // WINBIO_E_DATABASE_NO_RESULTS
}
static u32 MS st_FirstRecord(void*pipe){
  fprintf(stderr,"[storage] FirstRecord -> %s\n", g_have_record?"S_OK":"NO_RESULTS");
  return g_have_record ? 0 : 0x8009801f;
}
static u32 MS st_NextRecord(void*pipe){
  fprintf(stderr,"[storage] NextRecord -> NO_MORE_RECORDS\n");
  return 0x80098020; // single-record store: always end after the first
}
static u32 MS st_GetCurrentRecord(void*pipe,u8*rec){
  fprintf(stderr,"[storage] GetCurrentRecord (tsize=%lu)\n",(unsigned long)g_rec_tsize);
  memset(rec,0,0x40);
  *(u64*)(rec+0x00)=(u64)g_rec_identity;
  *(u64*)(rec+0x20)=(u64)g_rec_template;
  *(u64*)(rec+0x28)=g_rec_tsize;
  return 0;
}
static u32 MS st_AddRecord(void*pipe,u8*rec,u8 sub){
  u64 tp=*(u64*)(rec+0x20), ts=*(u64*)(rec+0x28);
  u64 idp=*(u64*)(rec+0x00);
  fprintf(stderr,"[storage] AddRecord(sub=%u) template=%#lx size=%lu\n",sub,(unsigned long)tp,(unsigned long)ts);
  if(idp) memcpy(g_rec_identity,(void*)idp,sizeof g_rec_identity);
  if(tp && ts && ts<=sizeof g_rec_template){ memcpy(g_rec_template,(void*)tp,ts); g_rec_tsize=ts; g_have_record=1; }
  return 0;
}

/* 0x200 bytes (64 slots), not 0x100: the engine's Verify path dispatches a storage
 * method at vtable offset 0x100 — one slot past a 0x100-sized array. A too-small
 * array read past-the-end (harmless st_default when adjacent to g_sensor_vt in the
 * standalone harness, but garbage 0x1 inside libfprint => call 0x1 => SIGSEGV). Size
 * generously and fill every slot with st_default so any offset the engine dispatches
 * is a safe no-op returning 0. */
static u64 g_storage_vt[0x200/8];
static u64 g_sensor_vt[0x200/8];
static void build_storage_vtable(void){
  for(int i=0;i<0x200/8;i++){ g_storage_vt[i]=(u64)st_default; g_sensor_vt[i]=(u64)st_default; }
  g_storage_vt[0x68/8]=(u64)st_AddRecord;
  g_storage_vt[0x78/8]=(u64)st_QueryBySubject;
  g_storage_vt[0x90/8]=(u64)st_FirstRecord;
  g_storage_vt[0x98/8]=(u64)st_NextRecord;
  g_storage_vt[0xa0/8]=(u64)st_GetCurrentRecord;
}


/* ==================== public API (ft_engine.h) ==================== */
#include "eg_engine.h"
#define ENG_W 70
#define ENG_H 57
static void *g_iface;

void ft_engine_geometry(int *w,int *h){ if(w)*w=ENG_W; if(h)*h=ENG_H; }

static int g_loaded = 0;
int ft_engine_open(const char *dll_path){
  /* Re-arm the %gs TEB on re-open: arch_prctl is per-thread and fprintd
   * re-opens the device (often on a different thread) between operations. */
  if(g_loaded){ set_gs(g_teb); return 0; }
  g_dllpath = dll_path ? dll_path : "ftWbioEngineAdapter.dll";
  register_shims();
  if(load_pe()!=0) return -1;
  void *q=get_export("WbioQueryEngineInterface");
  if(!q) return -2;
  u64 (MS *Query)(void**)=q;
  void *iface=0;
  if((u32)Query(&iface)!=0 || !iface) return -3;
  g_iface=iface;
  /* ---- DEBUG: dump the engine interface header + function table as RVAs ---- */
  fprintf(stderr,"[iface] @%p  header qwords:",iface);
  for(int i=0;i<4;i++) fprintf(stderr," %#lx",*(u64*)((u8*)iface+i*8));
  fprintf(stderr,"\n[iface] fn table (iface+0x20 + i*8) as RVAs:\n");
  for(int i=0;i<24;i++){ u64 p=*(u64*)((u8*)iface+32+i*8);
    u64 rva=(p>=g_imagebase && p<g_imagebase+0x100000)?(p-g_imagebase):0;
    fprintf(stderr,"   [%2d] %#18lx  rva=%#-8lx\n",i,p,rva); }
  *(u64*)(pipeline+0x00)=0x808;   /* Egis Attach uses pipeline[0] as the device handle */
  *(u64*)(pipeline+0x08)=~0ULL;
  build_storage_vtable();
  *(u64*)(pipeline+0x20)=(u64)g_sensor_vt;
  *(u64*)(pipeline+0x28)=(u64)g_storage_vt;
  u64 (MS *Attach)(void*)=*(void**)((u8*)iface+32+0*8);
  fprintf(stderr,"[open] calling slot0 (Attach hypothesis) @%#lx ...\n",(u64)Attach);
  u32 ar=(u32)Attach(pipeline);
  fprintf(stderr,"[open] Attach -> %#x; pipeline+0x38=%#lx\n",ar,*(u64*)(pipeline+0x38));
  if(ar!=0) return -4;
  u64 ctx=*(u64*)(pipeline+0x38);
  if(ctx>=0x1000){ *(u32*)(ctx+0x24)=ENG_W; *(u32*)(ctx+0x28)=ENG_H; }
  else fprintf(stderr,"[open] ctx(pipeline+0x38)=%#lx not a ptr — skipping geometry write\n",ctx);
  g_loaded=1;
  return 0;
}

void ft_engine_close(void){
  /* The engine maps at a fixed base and is loaded once per process; a libfprint
   * device close/reopen must NOT tear it down (and must not clear g_iface, or the
   * next open — which short-circuits on the load-once guard — leaves a null
   * interface and the following engine call segfaults). Intentional no-op. */
}

uint32_t ft_engine_accept(const uint8_t *img,int sw,int sh,uint8_t purpose){
  set_gs(g_teb);   /* ensure %gs points at our TEB on this thread */
  prepare_frame((const u8*)img, sw, sh);
  int total=build_bir(small, ENG_W, ENG_H);
  u32 rej=0;
  u64 (MS *Accept)(void*,void*,u64,u64,void*)=*(void**)((u8*)g_iface+32+8*8);
  return (u32)Accept(pipeline, birbuf, total, purpose, &rej);
}

/* raw accept: build the BIR at an explicit w x h (no crop/scale) — geometry probe */
uint32_t ft_engine_accept_raw(const uint8_t *img,int w,int h,uint8_t purpose){
  set_gs(g_teb);
  if((long)w*h > (long)sizeof birbuf - 0x80) return 0xE0000001;
  int total=build_bir((u8*)img,w,h);
  u32 rej=0;
  u64 (MS *Accept)(void*,void*,u64,u64,void*)=*(void**)((u8*)g_iface+32+8*8);
  return (u32)Accept(pipeline, birbuf, total, purpose, &rej);
}

void ft_engine_enroll_begin(void){
  set_gs(g_teb);
  g_have_record=0; g_rec_tsize=0;
  u64 (MS *Create)(void*)=*(void**)((u8*)g_iface+32+12*8);
  Create(pipeline);
}

int ft_engine_enroll_update(void){
  set_gs(g_teb);
  u64 (MS *Update)(void*,void*)=*(void**)((u8*)g_iface+32+13*8);
  u64 (MS *Status)(void*,void*)=*(void**)((u8*)g_iface+32+14*8);
  u32 urej=0; u64 uhr=Update(pipeline,&urej);
  if((u32)uhr==0x80098008 || urej) return 2;   /* frame rejected (dup/low quality) */
  u32 srej=0; u64 shr=Status(pipeline,&srej);
  return (u32)shr==0 ? 0 : 1;                    /* 0 complete, 1 need more */
}

static void fill_identity(u8 *id){ memset(id,0,76); *(u32*)id=3; for(int i=0;i<16;i++) id[8+i]=0xA0+i; }

int ft_engine_enroll_commit(uint8_t **out,size_t *outlen){
  set_gs(g_teb);
  u8 id[76]; fill_identity(id);
  g_have_record=0;
  u64 (MS *Commit)(void*,void*,u8)=*(void**)((u8*)g_iface+32+17*8);
  if((u32)Commit(pipeline,id,1)!=0 || !g_have_record) return -1;
  *out=malloc(g_rec_tsize); if(!*out) return -2;
  memcpy(*out, g_rec_template, g_rec_tsize); *outlen=g_rec_tsize;
  return 0;
}

int ft_engine_verify(const uint8_t *tmpl,size_t tmpllen){
  set_gs(g_teb);
  if(tmpllen==0 || tmpllen>sizeof g_rec_template) return 0;
  memcpy(g_rec_template, tmpl, tmpllen); g_rec_tsize=tmpllen; g_have_record=1;
  u8 id[76]; fill_identity(id);
  u8 m=0; u32 ps=0,hs=0,vr=0; void*plp=0,*hp=0;
  u64 (MS *Verify)(void*,void*,u8,void*,void*,void*,void*,void*,void*)=*(void**)((u8*)g_iface+32+10*8);
  Verify(pipeline, id, 1, &m, &ps, &plp, &hs, &hp, &vr);
  return m?1:0;
}
