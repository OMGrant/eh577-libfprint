/* poc_test.c — validate the C POC matcher on the real corpus; compare to Python.
 * Offline, no hardware. Reproduces the genuine/impostor separation experiment. */
#include "poc_match.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>

#define MAXF 80
typedef struct { uint8_t f[MAXF][POC_W*POC_H]; int n; } frameset;

static int cmpstr(const void *a, const void *b){ return strcmp(*(char**)a,*(char**)b); }

static void load_dir(const char *dir, frameset *fs) {
    fs->n = 0;
    char *names[512]; int nn = 0;
    DIR *d = opendir(dir); if (!d) { fprintf(stderr,"no dir %s\n",dir); return; }
    struct dirent *e;
    while ((e = readdir(d)) && nn < 512) {
        const char *dot = strrchr(e->d_name, '.');
        if (dot && !strcmp(dot, ".raw")) names[nn++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, nn, sizeof(char*), cmpstr);
    for (int i = 0; i < nn && fs->n < MAXF; i++) {
        char path[1024]; snprintf(path,sizeof path,"%s/%s",dir,names[i]);
        FILE *fp = fopen(path,"rb");
        if (fp) {
            uint8_t buf[POC_W*POC_H];
            if (fread(buf,1,sizeof buf,fp)==sizeof buf && poc_finger_present(buf))
                memcpy(fs->f[fs->n++], buf, sizeof buf);
            fclose(fp);
        }
        free(names[i]);
    }
}

static void stats(double *v, int n, double *mean, double *sd) {
    double m=0; for(int i=0;i<n;i++) m+=v[i]; m/=n;
    double s=0; for(int i=0;i<n;i++){double d=v[i]-m; s+=d*d;}
    *mean=m; *sd=sqrt(s/n);
}

int main(int argc, char **argv) {
    const char *base = "captures";
    const char *fingers[] = {"fingerA","fingerB","live_confirmed"};
    int NF = 3;
    frameset fset[3];
    for (int i=0;i<NF;i++){
        char p[1024]; snprintf(p,sizeof p,"%s/%s",base,fingers[i]);
        load_dir(p,&fset[i]);
        printf("  %s: %d finger frames\n", fingers[i], fset[i].n);
    }
    printf("\n");

    /* gallery = first half, probe = second half */
    int rots1[] = {0};
    int rots7[] = {-18,-12,-6,0,6,12,18};

    int radii[] = {24, 48, 64};
    for (int ri=0; ri<3; ri++) {
        int radius = radii[ri];
        for (int rp=0; rp<2; rp++) {
            int *rots = rp?rots7:rots1; int nr = rp?7:1;
            /* build galleries */
            poc_spectrum *gal[3]; int gn[3];
            for (int i=0;i<NF;i++){
                gn[i] = fset[i].n/2;
                gal[i] = malloc(sizeof(poc_spectrum)*gn[i]);
                for (int k=0;k<gn[i];k++) poc_prepare(fset[i].f[k], &gal[i][k]);
            }
            double gen[4096], imp[4096]; int ng=0, ni=0;
            for (int i=0;i<NF;i++){
                for (int k=fset[i].n/2; k<fset[i].n; k++){
                    gen[ng++] = poc_gallery_best(gal[i], gn[i], fset[i].f[k], radius, rots, nr);
                    for (int j=0;j<NF;j++) if (j!=i)
                        imp[ni++] = poc_gallery_best(gal[j], gn[j], fset[i].f[k], radius, rots, nr);
                }
            }
            double gm,gs,im,is; stats(gen,ng,&gm,&gs); stats(imp,ni,&im,&is);
            double dp = fabs(gm-im)/sqrt(0.5*(gs*gs+is*is)+1e-12);
            printf("[C radius=%2d %-9s] genuine=%.3f±%.3f impostor=%.3f±%.3f  d'=%5.1f  (%d gen,%d imp)\n",
                   radius, rp?"rot±18":"no-rot", gm,gs,im,is,dp,ng,ni);
            for (int i=0;i<NF;i++) free(gal[i]);
        }
        printf("\n");
    }
    return 0;
}
