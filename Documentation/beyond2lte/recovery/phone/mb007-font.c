// SPDX-License-Identifier: GPL-2.0
/* Scale the current tty1 bitmap font 2x without the legacy 32-row ioctl limit. */
#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#ifndef KD_FONT_OP_SET_TALL
#define KD_FONT_OP_SET_TALL 4
#define KD_FONT_OP_GET_TALL 5
#endif
#define MAX_WIDTH 64U
#define MAX_HEIGHT 128U
#define MAX_GLYPHS 512U

static int get_font(int fd, struct console_font_op *f, unsigned char *data)
{
    memset(f, 0, sizeof(*f));
    f->op = KD_FONT_OP_GET_TALL;
    f->width = MAX_WIDTH;
    f->height = MAX_HEIGHT;
    f->charcount = MAX_GLYPHS;
    f->data = data;
    if (ioctl(fd, KDFONTOP, f) < 0) { perror("KDFONTOP GET_TALL"); return -1; }
    return 0;
}

int main(int argc, char **argv)
{
    struct console_font_op src, dst, actual;
    unsigned char *in = NULL, *out = NULL;
    int rc = 1;
    if (argc != 2 || (strcmp(argv[1], "--inspect") && strcmp(argv[1], "--double"))) {
        fprintf(stderr, "usage: %s --inspect|--double\n", argv[0]); return 2;
    }
    int fd = open("/dev/tty1", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("/dev/tty1"); return 3; }
    in = calloc(MAX_GLYPHS * MAX_HEIGHT * (MAX_WIDTH / 8U), 1);
    if (!in) { perror("calloc"); goto done; }
    if (get_font(fd, &src, in)) goto done;
    printf("before=%ux%u glyphs=%u\n", src.width, src.height, src.charcount);
    if (!strcmp(argv[1], "--inspect")) { rc = 0; goto done; }
    if (src.width == 32 && src.height == 64) { puts("already=32x64"); rc = 0; goto done; }
    if (src.width != 16 || src.height != 32 ||
        (src.charcount != 256 && src.charcount != 512)) {
        fprintf(stderr, "refused: expected 16x32 source font, got %ux%u/%u\n",
                src.width, src.height, src.charcount); goto done;
    }
    unsigned int in_pitch = (src.width + 7) / 8, out_pitch = (2 * src.width + 7) / 8;
    size_t out_size = (size_t)src.charcount * src.height * 2 * out_pitch;
    out = calloc(out_size, 1);
    if (!out) { perror("calloc output"); goto done; }
    /* GET_TALL returns each glyph with the requested vpitch (MAX_HEIGHT). */
    for (unsigned int g = 0; g < src.charcount; g++) {
        for (unsigned int y = 0; y < src.height; y++) {
            const unsigned char *row = in + (size_t)g * MAX_HEIGHT * in_pitch + y * in_pitch;
            for (unsigned int x = 0; x < src.width; x++) {
                if (!(row[x / 8] & (0x80U >> (x % 8)))) continue;
                for (unsigned int dy = 0; dy < 2; dy++)
                    for (unsigned int dx = 0; dx < 2; dx++) {
                        unsigned int xx = 2 * x + dx;
                        out[((size_t)g * (2 * src.height) + 2 * y + dy) * out_pitch + xx / 8] |= (unsigned char)(0x80U >> (xx % 8));
                    }
            }
        }
    }
    memset(&dst, 0, sizeof(dst));
    dst.op = KD_FONT_OP_SET_TALL;
    dst.width = src.width * 2;
    dst.height = src.height * 2;
    dst.charcount = src.charcount;
    dst.data = out;
    if (ioctl(fd, KDFONTOP, &dst) < 0) { perror("KDFONTOP SET_TALL"); goto done; }
    if (get_font(fd, &actual, in)) goto done;
    printf("after=%ux%u glyphs=%u\n", actual.width, actual.height, actual.charcount);
    if (actual.width != dst.width || actual.height != dst.height || actual.charcount != dst.charcount) {
        fprintf(stderr, "font dimensions do not match\n"); goto done;
    }
    /* Also compare actual bitmap bytes, not just dimensions. */
    for (unsigned int g = 0; g < dst.charcount; g++)
        for (unsigned int y = 0; y < dst.height; y++)
            if (memcmp(in + ((size_t)g * MAX_HEIGHT + y) * out_pitch,
                       out + ((size_t)g * dst.height + y) * out_pitch, out_pitch)) {
                fprintf(stderr, "font bitmap differs at glyph %u row %u\n", g, y); goto done;
            }
    puts("bitmap-readback=matched");
    rc = 0;
done:
    free(in); free(out); close(fd); return rc;
}
