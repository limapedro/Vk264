#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slicetype.h"

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "build/long64.yuv";
    int n = argc > 2 ? atoi(argv[2]) : 40, w = 64, h = 64;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    size_t fs = (size_t)w*h*3/2;
    uint8_t *frames = malloc(fs*n);
    if (fread(frames, fs, n, f) != (size_t)n) { fprintf(stderr, "short input\n"); return 2; }
    fclose(f);
    vk_frame_t *t = calloc(n, sizeof(*t));
    int *order = calloc(n, sizeof(*order)), nc = 0;
    vk_slicetype_decide(frames, n, w, h, 30, 25, 3, 2, 40, t, order, &nc);
    printf("coding order (%d):", nc);
    for (int i = 0; i < nc; ++i) printf(" %d", order[i]);
    printf("\n");
    for (int i = 0; i < nc; ++i) {
        int d = order[i];
        printf("c=%d d=%d type=%d fn=%d poc=%d sc=%d\n", i, d,
               (int)t[d].type, t[d].frame_num, t[d].poc, t[d].is_scenecut);
    }
    return 0;
}
