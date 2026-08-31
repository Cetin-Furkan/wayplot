#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

/* Fail the build if Slang puts a RowMajor matrix in the UBO. That decoration
 * transposes CPU column-major clip matrices and is how a cube becomes a dart.
 * lit.slang stores four float4 columns on purpose so this scan should see
 * zero matrices; either way RowMajor is fatal. */

static const unsigned char vert_spv[] __attribute__((aligned(4))) = {
#embed "shaders/lit.vert.spv"
};
static const unsigned char frag_spv[] __attribute__((aligned(4))) = {
#embed "shaders/lit.frag.spv"
};

static int g_fail;

static void expect(int cond, const char *what)
{
    if (cond)
        printf("PASS  %s\n", what);
    else {
        printf("FAIL  %s\n", what);
        g_fail++;
    }
}

enum {
    SpvOpTypeMatrix = 24,
    SpvOpDecorate = 71,
    SpvDecorationRowMajor = 4,
    SpvDecorationColMajor = 5,
};

static int scan(const char *tag, const unsigned char *bytes, size_t nbytes,
                int *row_major, int *col_major, int *matrices)
{
    const uint32_t *w = (const uint32_t *)bytes;
    size_t nwords = nbytes / 4;
    size_t i;

    *row_major = 0;
    *col_major = 0;
    *matrices = 0;
    if (nbytes < 20 || (nbytes & 3u) != 0 || w[0] != 0x07230203u) {
        printf("FAIL  %s: not SPIR-V (%zu bytes)\n", tag, nbytes);
        g_fail++;
        return -1;
    }
    for (i = 5; i < nwords; ) {
        uint32_t wc = w[i] >> 16;
        uint32_t op = w[i] & 0xffffu;
        if (wc == 0 || i + wc > nwords) {
            printf("FAIL  %s: truncated SPIR-V at word %zu\n", tag, i);
            g_fail++;
            return -1;
        }
        if (op == SpvOpTypeMatrix)
            (*matrices)++;
        if (op == SpvOpDecorate && wc >= 3) {
            uint32_t dec = w[i + 2];
            if (dec == SpvDecorationRowMajor)
                (*row_major)++;
            if (dec == SpvDecorationColMajor)
                (*col_major)++;
        }
        i += wc;
    }
    printf("      %s  RowMajor %d  ColMajor %d  OpTypeMatrix %d\n",
           tag, *row_major, *col_major, *matrices);
    return 0;
}

int main(void)
{
    int vr = 0, vc = 0, vm = 0, fr = 0, fc = 0, fm = 0;

    expect(sizeof(vert_spv) > 64 && sizeof(frag_spv) > 64, "embedded lit SPIR-V is non-empty");
    if (scan("lit.vert", vert_spv, sizeof(vert_spv), &vr, &vc, &vm) == 0) {
        expect(vr == 0, "vertex SPIR-V has no RowMajor matrix (would transpose CPU clip)");
        expect(vm == 0, "vertex SPIR-V has no float4x4 in the module (UBO is four columns)");
    }
    if (scan("lit.frag", frag_spv, sizeof(frag_spv), &fr, &fc, &fm) == 0)
        expect(fr == 0, "fragment SPIR-V has no RowMajor matrix");

    printf("%s  %d failure(s)\n", g_fail ? "RESULT FAIL" : "RESULT PASS", g_fail);
    return g_fail ? 1 : 0;
}
