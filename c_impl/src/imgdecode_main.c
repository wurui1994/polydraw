/* pd-imgdecode — decode any image file (JPEG/PNG/BMP/etc) to PPM on stdout,
 * using the same stb_image decoder as the C renderer, so JS softrender loads
 * exactly the same pixels. */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "stb_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s image.jpg|png|bmp [out.ppm]\n", argv[0]);
        return 1;
    }
    int w, h, ch;
    unsigned char *px = stbi_load(argv[1], &w, &h, &ch, 4);
    if (!px) {
        fprintf(stderr, "stbi_load failed: %s\n", stbi_failure_reason());
        return 1;
    }
    const char *out = argc > 2 ? argv[2] : NULL;
    FILE *fp = out ? fopen(out, "wb") : stdout;
    if (!fp) { fprintf(stderr, "cannot open output\n"); stbi_image_free(px); return 1; }
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++)
        fprintf(fp, "%c%c%c", px[i*4], px[i*4+1], px[i*4+2]);
    if (out) fclose(fp);
    stbi_image_free(px);
    return 0;
}
