/*
 * The group interpreter against the scalar one. Real shader binaries (deko3d
 * .dksh files, given as arguments) are started at every 32-byte boundary, so
 * real paths and arbitrary words are both covered, with different random
 * registers, predicates and interpolants in each of the eight lanes and random
 * active masks; half the trials share predicates across lanes, the other half
 * diverge. Every active lane must end exactly as the scalar interpreter leaves
 * it, and a lane that fails must fail the group. Without binaries there is
 * nothing to compare and the test passes.
 *
 *   test_shader_group [file.dksh...]     (TRIALS=n per start, default 8)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_shader.h"

static uint64_t g_rng = 0x2545F4914F6CDD1Dull;

static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)g_rng;
}

static uint32_t mix(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u);
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

static float fl(uint32_t v) {
    float x;
    memcpy(&x, &v, 4);
    return x;
}

static uint32_t bits_of(float x) {
    uint32_t v;
    memcpy(&v, &x, 4);
    return v;
}

/* Random words, or small floats so arithmetic stays in a meaningful range. */
static uint32_t value(void) {
    return (rnd() & 3) ? rnd() : bits_of((float)(rnd() % 200) / 50.0f - 2.0f);
}

/* Callbacks: pure functions of their inputs, so both interpreters see the same. */
static uint32_t g_seed[SHADER_LANES];

static float ipa_scalar(void *user, uint32_t addr, uint32_t mode) {
    return fl(mix(*(const uint32_t *)user, mix(addr, mode)) & 0x3FFFFFFF);
}

static float ipa_group(void *user, uint32_t lane, uint32_t addr, uint32_t mode) {
    (void)user;
    return fl(mix(g_seed[lane], mix(addr, mode)) & 0x3FFFFFFF);
}

static void tex(void *user, uint32_t index, float u, float v, float out[4]) {
    (void)user;
    uint32_t h = mix(index, mix(bits_of(u), bits_of(v)));
    for (int c = 0; c < 4; c++) {
        out[c] = fl(mix(h, (uint32_t)c) & 0x3FFFFFFF);
    }
}

static void texel(void *user, uint32_t index, int32_t x, int32_t y, float out[4]) {
    tex(user, index ^ 0x5555, fl((uint32_t)x), fl((uint32_t)y), out);
}

static uint8_t g_cbuf[SHADER_CBUFS][0x1000];
static uint8_t g_file[1 << 20];
static shader_env g_tmpl, g_lane[SHADER_LANES];
static shader_group g_group;

/* One trial: returns 1 when every lane agrees. */
static int trial(const shader_prog *prog, int shared_preds, uint32_t active) {
    memset(&g_tmpl, 0, sizeof(g_tmpl));
    for (int c = 0; c < SHADER_CBUFS; c++) {
        for (int k = 0; k < 0x1000; k += 4) {
            uint32_t v = (rnd() & 1) ? rnd() : bits_of((float)(rnd() % 100) / 10.0f);
            memcpy(g_cbuf[c] + k, &v, 4);
        }
        g_tmpl.cb[c] = g_cbuf[c];
        g_tmpl.cb_size[c] = (rnd() & 7) ? 0x1000 : 0x100;
    }
    g_tmpl.tex = tex;
    g_tmpl.texel = texel;
    memset(&g_group, 0, sizeof(g_group));
    uint32_t preds = rnd();
    for (uint32_t l = 0; l < SHADER_LANES; l++) {
        shader_env *e = &g_lane[l];
        *e = g_tmpl;
        g_seed[l] = rnd();
        for (int k = 0; k < 255; k++) {
            e->r[k] = value();
        }
        for (int k = 0; k < 8; k++) {
            e->p[k] = (uint8_t)(shared_preds ? (preds >> k) & 1 : rnd() & 1);
        }
        e->carry = rnd() & 1;
        e->ipa = ipa_scalar;
        e->user = &g_seed[l];
        for (int k = 0; k < 260; k++) {
            g_group.r[k][l] = e->r[k];
        }
        for (int k = 0; k < 9; k++) {
            g_group.p[k] |= (uint32_t)(e->p[k] & 1) << l;
        }
        g_group.carry |= e->carry << l;
    }
    g_group.ipa = ipa_group;
    uint64_t bad = 0;
    int group_rc = shader_exec_group(prog, &g_group, active, &g_tmpl, &bad);
    int lane_rc[SHADER_LANES] = {0}, any_fail = 0;
    for (uint32_t l = 0; l < SHADER_LANES; l++) {
        if ((active >> l) & 1) {
            lane_rc[l] = shader_exec(prog, &g_lane[l], &bad);
            any_fail |= lane_rc[l] != 0;
        }
    }
    if (any_fail) {
        return group_rc != 0; /* the group stops at a failing lane; its other lanes are moot */
    }
    if (group_rc != 0) {
        return 0;
    }
    for (uint32_t l = 0; l < SHADER_LANES; l++) {
        if (!((active >> l) & 1)) {
            continue;
        }
        const shader_env *e = &g_lane[l];
        for (int k = 0; k < 255; k++) {
            if (g_group.r[k][l] != e->r[k]) {
                printf("  lane %u r%d: group %#x, scalar %#x\n", l, k, g_group.r[k][l], e->r[k]);
                return 0;
            }
        }
        for (int k = 0; k < 7; k++) {
            if ((int)((g_group.p[k] >> l) & 1) != e->p[k]) {
                printf("  lane %u p%d differs\n", l, k);
                return 0;
            }
        }
        if (((g_group.carry >> l) & 1) != e->carry) {
            printf("  lane %u carry differs\n", l);
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    int trials = getenv("TRIALS") ? atoi(getenv("TRIALS")) : 8;
    unsigned starts = 0, groups = 0, bad = 0;
    for (int a = 1; a < argc; a++) {
        FILE *fp = fopen(argv[a], "rb");
        if (!fp) {
            printf("cannot open %s\n", argv[a]);
            return 1;
        }
        size_t len = fread(g_file, 1, sizeof(g_file), fp);
        fclose(fp);
        for (size_t off = 0; off + 64 <= len; off += 32) {
            const uint64_t *code = (const uint64_t *)(g_file + off);
            shader_prog *prog = shader_prog_get(code, (uint32_t)((len - off) / 8));
            if (!prog) {
                return 1;
            }
            starts++;
            for (int t = 0; t < trials; t++) {
                uint32_t active =
                    t % 4 == 0 ? (1u << SHADER_LANES) - 1 : rnd() & ((1u << SHADER_LANES) - 1);
                groups++;
                if (!trial(prog, t & 1, active) && bad++ < 10) {
                    printf("mismatch: %s +%#zx, trial %d, active %#x\n", argv[a], off, t, active);
                }
            }
            shader_prog_release(prog);
        }
    }
    printf("%u starts, %u groups, %u mismatches\n", starts, groups, bad);
    return bad != 0;
}
