#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slicetype.h"

/* Self-contained fixture: synthetic frames (flat, scene change at frame 8). */
int main(int argc, char **argv)
{
    const int n=argc>2?atoi(argv[2]):16, w=64, h=64;
    size_t fs=(size_t)w*h*3/2;
    uint8_t *frames=malloc(fs*n);
    if (argc>1) {
        FILE *f=fopen(argv[1],"rb");
        if(!f){fprintf(stderr,"cannot open %s\n",argv[1]);return 2;}
        if(fread(frames,fs,n,f)!=(size_t)n){fprintf(stderr,"short input\n");return 2;}
        fclose(f);
    } else {
    for (int i=0;i<n;++i) {
        uint8_t v = (i>=8) ? 200 : 16;   /* scene change */
        memset(frames+(size_t)i*fs, v, fs);
    }
    }
    vk_frame_t *t=calloc(n,sizeof(*t));
    int *order=calloc(n,sizeof(*order)), nc=0;
    vk_slicetype_decide(frames,n,w,h,30,25,3,2,40,t,order,&nc);
    printf("coding order (%d):",nc);
    for (int i=0;i<nc;++i) printf(" %d",order[i]);
    printf("\n");
    int scenecuts=0;
    for (int i=0;i<nc;++i) {
        int d=order[i];
        printf("c=%d d=%d type=%d fn=%d poc=%d sc=%d\n",i,d,
               (int)t[d].type,t[d].frame_num,t[d].poc,t[d].is_scenecut);
        scenecuts+=t[d].is_scenecut;
    }
    if (argc>1) { free(frames);free(t);free(order);return 0; } /* file mode: dump only */
    /* Basic acceptance: all frames coded exactly once, IDR at 0 and 8,
     * POC monotonic in display order, scenecut detected at 8. */
    if (nc!=n) { fprintf(stderr,"FAIL coding count %d != %d\n",nc,n); return 1; }
    if (t[0].type!=VK_FRAME_IDR) { fprintf(stderr,"FAIL frame0 not IDR\n"); return 1; }
    if (t[8].type!=VK_FRAME_IDR) { fprintf(stderr,"FAIL frame8 not IDR\n"); return 1; }
    if (!t[8].is_scenecut) { fprintf(stderr,"FAIL scenecut not detected\n"); return 1; }
    for (int i=1;i<n;++i)
        if (t[i].type!=VK_FRAME_IDR && t[i].poc<=t[i-1].poc &&
            t[i-1].type!=VK_FRAME_IDR) {
            fprintf(stderr,"FAIL POC order at %d\n",i); return 1;
        }
    printf("slicetype fixture PASS: %d scenecuts\n",scenecuts);
    free(frames);free(t);free(order);
    return 0;
}
