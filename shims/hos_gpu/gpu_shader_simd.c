/*
 * The interpreter for a group of fragments (SHADER_LANES): each decoded
 * instruction is dispatched once and applied to every lane its guard enables,
 * so the decode and dispatch cost is shared by the group. Registers are laid
 * out lane by lane (r[reg][lane]) and predicates are lane masks.
 *
 * Handlers mirror the scalar ones in gpu_shader.c lane for lane. While every
 * live lane takes the same path, branches and reconvergence are uniform; a
 * branch or SYNC that splits the group, or an instruction this file does not
 * handle, hands each lane to the scalar interpreter at that pc with the
 * group's control-flow state, so results equal running each fragment alone.
 */
#include <stdlib.h>

#include "gpu_shader_impl.h"

#define LANES SHADER_LANES
#define FOR_LANES(m)                                                                               \
    for (uint32_t l = 0; l < LANES; l++)                                                           \
        if (((m) >> l) & 1)

static INLINE void gset(shader_group *g, uint32_t n, uint32_t l, uint32_t v) {
    g->r[n >= RZ ? 256 : n][l] = v;
}

static INLINE int gpred(const shader_group *g, uint32_t n, uint32_t l) {
    return (int)((g->p[n & 7] >> l) & 1);
}

static INLINE void gset_pred(shader_group *g, uint32_t n, uint32_t l, int v) {
    uint32_t k = (n & 7) == PT ? 8 : (n & 7);
    g->p[k] = (g->p[k] & ~(1u << l)) | ((uint32_t)(v != 0) << l);
}

static INLINE uint32_t gcbuf(const shader_env *e, uint32_t idx, uint32_t byte_off) {
    if (idx >= SHADER_CBUFS || !e->cb[idx] || byte_off + 4 > e->cb_size[idx]) {
        return 0;
    }
    uint32_t v;
    memcpy(&v, e->cb[idx] + byte_off, 4);
    return v;
}

static INLINE uint32_t gopnd_b(const shader_group *g, const shader_env *e, const pinsn *in,
                               uint32_t l) {
    if (in->bk == BK_REG) {
        return g->r[in->rb][l];
    }
    if (in->bk == BK_CBUF) {
        return gcbuf(e, in->cbi, in->b);
    }
    return in->b;
}

/*
 * Whole-group forms of the hot arithmetic: every lane is computed into a
 * temporary, without branches, and the enabled lanes are blended into the
 * destination, which the compiler turns into vector code. Negation and
 * absolute value are sign-bit operations, exactly what -x and fabsf do.
 */
static INLINE void put(shader_group *g, uint32_t d, uint32_t m, const uint32_t *v) {
    uint32_t *dst = g->r[d >= RZ ? 256 : d];
    for (uint32_t l = 0; l < LANES; l++) {
        dst[l] = ((m >> l) & 1) ? v[l] : dst[l];
    }
}

static INLINE void opnd_b_vec(const shader_group *g, const shader_env *e, const pinsn *in,
                              uint32_t *out) {
    if (in->bk == BK_REG) {
        memcpy(out, g->r[in->rb], sizeof(uint32_t) * LANES);
        return;
    }
    uint32_t v = in->bk == BK_CBUF ? gcbuf(e, in->cbi, in->b) : in->b;
    for (uint32_t l = 0; l < LANES; l++) {
        out[l] = v;
    }
}

static INLINE float satv(float x) {
    return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x != x ? 0.0f : x;
}

/* One lane on the scalar interpreter: IPA goes back to the group's callback. */
typedef struct lane_ipa {
    const shader_group *g;
    uint32_t lane;
} lane_ipa;

static float lane_ipa_fn(void *user, uint32_t addr, uint32_t mode) {
    const lane_ipa *t = user;
    return t->g->ipa ? t->g->ipa(t->g->user, t->lane, addr, mode) : 0.0f;
}

static int run_lanes(const shader_prog *prog, shader_group *g, uint32_t live,
                     const shader_env *tmpl, uint32_t pc, const shader_cf *cf, uint64_t *bad_insn) {
    shader_env *e = malloc(sizeof(*e));
    if (!e) {
        *bad_insn = 0;
        return -1;
    }
    FOR_LANES(live) {
        memcpy(e, tmpl, sizeof(*e));
        for (uint32_t n = 0; n < 260; n++) {
            e->r[n] = g->r[n][l];
        }
        for (uint32_t k = 0; k < 9; k++) {
            e->p[k] = (uint8_t)((g->p[k] >> l) & 1);
        }
        e->carry = (g->carry >> l) & 1;
        lane_ipa t = {g, l};
        e->ipa = lane_ipa_fn;
        e->user = &t;
        shader_cf c = *cf;
        int rc = shader_exec_from(prog, e, pc, &c, bad_insn);
        for (uint32_t n = 0; n < 260; n++) {
            g->r[n][l] = e->r[n];
        }
        for (uint32_t k = 0; k < 9; k++) {
            g->p[k] = (g->p[k] & ~(1u << l)) | ((uint32_t)e->p[k] << l);
        }
        g->carry = (g->carry & ~(1u << l)) | (e->carry << l);
        if (rc != 0) {
            free(e);
            return rc;
        }
    }
    free(e);
    return 0;
}

/*
 * Coordinates of a texture instruction for the lanes in `m`, with the screen
 * derivatives the 2x2 quad gives: lanes 4q..4q+3 are the quad's pixels in
 * reading order, so a lane's x neighbour is the other lane of its row and its y
 * neighbour the lane two below or above. A quad with a lane missing from `m`
 * (one that left the group) has no derivatives.
 */
static void quad_coords(const shader_group *g, uint32_t m, uint32_t ru, uint32_t rv, int lod_mode,
                        shader_tex_coord tc[SHADER_LANES]) {
    for (uint32_t l = 0; l < SHADER_LANES; l++) {
        tc[l].u = f(g->r[ru][l]);
        tc[l].v = f(g->r[rv][l]);
        tc[l].lod_mode = lod_mode;
        tc[l].lod = 0.0f;
        tc[l].has_grad = 0;
        tc[l].dudx = tc[l].dvdx = tc[l].dudy = tc[l].dvdy = 0.0f;
    }
    for (uint32_t q = 0; q < SHADER_LANES; q += 4) {
        if (((m >> q) & 0xF) != 0xF) {
            continue;
        }
        for (uint32_t l = q; l < q + 4; l++) {
            uint32_t row = l & ~1u, col = q | (l & 1);
            tc[l].dudx = tc[row + 1].u - tc[row].u;
            tc[l].dvdx = tc[row + 1].v - tc[row].v;
            tc[l].dudy = tc[col + 2].u - tc[col].u;
            tc[l].dvdy = tc[col + 2].v - tc[col].v;
            tc[l].has_grad = 1;
        }
    }
}

int shader_exec_group(const shader_prog *prog, shader_group *g, uint32_t active,
                      const shader_env *env, uint64_t *bad_insn) {
    const pinsn *ins = prog->ins;
    uint32_t nwords = prog->n;
    uint32_t pc = 1; /* word 0 is the first control word */
    shader_cf cf;
    memset(&cf, 0, sizeof(cf));
    uint32_t(*stack)[CF_DEPTH] = cf.stack, *stack_n = cf.n;
    uint32_t live = active & ((1u << LANES) - 1);
    if (!live) {
        return 0;
    }
    for (uint32_t l = 0; l < LANES; l++) {
        g->r[RZ][l] = 0;
    }
    g->p[PT] = (1u << LANES) - 1;
    for (uint32_t step = 0; step < MAX_STEPS; step++) {
        if (pc >= nwords) {
            *bad_insn = 0;
            return -1;
        }
        const pinsn *in = &ins[pc];
        uint32_t next = pc + 1;
        uint32_t m = live;
        if (in->guard != PT) {
            uint32_t pm = g->p[in->guard & 7];
            m &= (in->guard & 8) ? ~pm : pm;
        }
        if (!m) {
            pc = next;
            continue;
        }
        u64 i = in->raw;
        uint32_t d = in->d, ra = in->ra;
        switch (in->op) {
        case OP_CTRL:
        case OP_NOP:
            break;
        case OP_EXIT:
            live &= ~m;
            if (!live) {
                return 0;
            }
            break;
        case OP_BRA: {
            if (m != live) {
                goto diverge;
            }
            int32_t off = (int32_t)(bits(i, 20, 24) << 8) >> 8;
            next = (uint32_t)(((int64_t)pc * 8 + 8 + off) / 8);
            break;
        }
        case OP_SSY: {
            if (m != live) {
                goto diverge;
            }
            uint32_t kind = (uint32_t)((i >> 52) & 0xF) - 9;
            int32_t off = (int32_t)(bits(i, 20, 24) << 8) >> 8;
            if (stack_n[kind] < CF_DEPTH) {
                stack[kind][stack_n[kind]++] = (uint32_t)(((int64_t)pc * 8 + 8 + off) / 8);
            }
            break;
        }
        case OP_SYNC: {
            if (m != live) {
                goto diverge;
            }
            uint32_t kind = (i >> 60) == 0xF ? 0 : (uint32_t)((i >> 52) & 0xF) - 3;
            if (stack_n[kind]) {
                next = kind == 2 ? stack[kind][stack_n[kind] - 1] : stack[kind][--stack_n[kind]];
            }
            break;
        }
        case OP_LDC: {
            int32_t off = (int32_t)(bits(i, 20, 16) << 16) >> 16;
            uint32_t idx = bits(i, 36, 5), size = bits(i, 48, 3);
            FOR_LANES(m) {
                uint32_t byte = g->r[ra][l] + (uint32_t)off;
                uint32_t w = gcbuf(env, idx, byte & ~3u);
                switch (size) {
                case 0:
                    gset(g, d, l, (w >> (8 * (byte & 3))) & 0xFF);
                    break;
                case 1:
                    gset(g, d, l, (uint32_t)(int32_t)(int8_t)(w >> (8 * (byte & 3))));
                    break;
                case 2:
                    gset(g, d, l, (w >> (8 * (byte & 2))) & 0xFFFF);
                    break;
                case 3:
                    gset(g, d, l, (uint32_t)(int32_t)(int16_t)(w >> (8 * (byte & 2))));
                    break;
                case 5:
                    gset(g, d, l, w);
                    gset(g, d == RZ ? RZ : d + 1, l, gcbuf(env, idx, (byte & ~3u) + 4));
                    break;
                default:
                    gset(g, d, l, w);
                    break;
                }
            }
            break;
        }
        case OP_IPA: {
            uint32_t mode = bits(i, 54, 2), rb = bits(i, 20, 8), sat_on = bits(i, 51, 1);
            FOR_LANES(m) {
                uint32_t addr = bits(i, 28, 10) + g->r[ra][l];
                float v = g->ipa ? g->ipa(g->user, l, addr, mode == 1 ? 1 : mode) : 0.0f;
                if (mode == 1) {
                    v *= f(g->r[rb][l]);
                }
                gset(g, d, l, u(sat(v, (int)sat_on)));
            }
            break;
        }
        case OP_RRO:
            FOR_LANES(m) {
                float y = fmod_neg_abs(f(gopnd_b(g, env, in, l)), bits(i, 45, 1), bits(i, 49, 1));
                gset(g, d, l, u(y));
            }
            break;
        case OP_F2I:
            FOR_LANES(m) {
                float y = fmod_neg_abs(f(gopnd_b(g, env, in, l)), bits(i, 45, 1), bits(i, 49, 1));
                double v = y != y ? 0.0 : (double)y;
                switch (bits(i, 39, 2)) {
                case 0:
                    v = nearbyint(v);
                    break;
                case 1:
                    v = floor(v);
                    break;
                case 2:
                    v = ceil(v);
                    break;
                default:
                    v = trunc(v);
                    break;
                }
                uint32_t out;
                if (bits(i, 12, 1)) {
                    out = (uint32_t)(int32_t)(v < -2147483648.0  ? -2147483648.0
                                              : v > 2147483647.0 ? 2147483647.0
                                                                 : v);
                } else {
                    out = (uint32_t)(v < 0.0 ? 0.0 : v > 4294967295.0 ? 4294967295.0 : v);
                }
                gset(g, d, l, out);
            }
            break;
        case OP_I2F:
            FOR_LANES(m) {
                uint32_t b = gopnd_b(g, env, in, l);
                float y = bits(i, 13, 1) ? (float)(int32_t)b : (float)b;
                gset(g, d, l, u(fmod_neg_abs(y, bits(i, 45, 1), bits(i, 49, 1))));
            }
            break;
        case OP_MUFU: {
            uint32_t fn = bits(i, 20, 4);
            if (fn > 5 && fn != 8) {
                goto diverge; /* the scalar interpreter reports it */
            }
            FOR_LANES(m) {
                float x = fmod_neg_abs(f(g->r[ra][l]), bits(i, 48, 1), bits(i, 46, 1)), r;
                switch (fn) {
                case 0:
                    r = cosf(x);
                    break;
                case 1:
                    r = sinf(x);
                    break;
                case 2:
                    r = exp2f(x);
                    break;
                case 3:
                    r = log2f(x);
                    break;
                case 4:
                    r = 1.0f / x;
                    break;
                case 5:
                    r = 1.0f / sqrtf(x);
                    break;
                default:
                    r = sqrtf(x);
                    break;
                }
                gset(g, d, l, u(sat(r, bits(i, 50, 1))));
            }
            break;
        }
        case OP_MOV32I: {
            uint32_t v = bits(i, 20, 32);
            FOR_LANES(m) {
                gset(g, d, l, v);
            }
            break;
        }
        case OP_MOV: {
            uint32_t bv[LANES];
            opnd_b_vec(g, env, in, bv);
            put(g, d, m, bv);
            break;
        }
        case OP_FADD: {
            uint32_t av[LANES], bv[LANES], rv[LANES];
            uint32_t aand = bits(i, 46, 1) ? 0x7FFFFFFFu : ~0u, axor = bits(i, 48, 1) << 31;
            uint32_t band = bits(i, 49, 1) ? 0x7FFFFFFFu : ~0u, bxor = bits(i, 45, 1) << 31;
            int st = bits(i, 50, 1);
            memcpy(av, g->r[ra], sizeof(av));
            opnd_b_vec(g, env, in, bv);
            for (uint32_t l = 0; l < LANES; l++) {
                float v = f((av[l] & aand) ^ axor) + f((bv[l] & band) ^ bxor);
                rv[l] = fres(st ? satv(v) : v);
            }
            put(g, d, m, rv);
            break;
        }
        case OP_FMUL: {
            uint32_t av[LANES], bv[LANES], rv[LANES];
            uint32_t bxor = bits(i, 48, 1) << 31;
            int st = bits(i, 50, 1);
            memcpy(av, g->r[ra], sizeof(av));
            opnd_b_vec(g, env, in, bv);
            for (uint32_t l = 0; l < LANES; l++) {
                float v = f(av[l]) * f(bv[l] ^ bxor);
                rv[l] = fres(st ? satv(v) : v);
            }
            put(g, d, m, rv);
            break;
        }
        case OP_FFMA: {
            uint32_t av[LANES], bv[LANES], cv[LANES], rv[LANES];
            uint32_t bxor = bits(i, 48, 1) << 31, cxor = bits(i, 49, 1) << 31;
            int st = bits(i, 50, 1);
            memcpy(av, g->r[ra], sizeof(av));
            opnd_b_vec(g, env, in, bv);
            if (in->ck) {
                uint32_t c = gcbuf(env, in->cbi, in->b);
                for (uint32_t l = 0; l < LANES; l++) {
                    cv[l] = c;
                }
            } else {
                memcpy(cv, g->r[in->rc], sizeof(cv));
            }
            for (uint32_t l = 0; l < LANES; l++) {
                float v = fmaf(f(av[l]), f(bv[l] ^ bxor), f(cv[l] ^ cxor));
                rv[l] = fres(st ? satv(v) : v);
            }
            put(g, d, m, rv);
            break;
        }
        case OP_FADD32I:
            FOR_LANES(m) {
                float x = fmod_neg_abs(f(g->r[ra][l]), bits(i, 56, 1), bits(i, 54, 1));
                float y = fmod_neg_abs(f(bits(i, 20, 32)), bits(i, 53, 1), bits(i, 57, 1));
                gset(g, d, l, fres(x + y));
            }
            break;
        case OP_FMUL32I:
            FOR_LANES(m) {
                gset(g, d, l, fres(sat(f(g->r[ra][l]) * f(bits(i, 20, 32)), bits(i, 55, 1))));
            }
            break;
        case OP_FFMA32I:
            FOR_LANES(m) {
                float x = fmod_neg_abs(f(g->r[ra][l]), bits(i, 56, 1), 0);
                float z = fmod_neg_abs(f(g->r[d][l]), bits(i, 57, 1), 0);
                gset(g, d, l, fres(sat(fmaf(x, f(bits(i, 20, 32)), z), bits(i, 55, 1))));
            }
            break;
        case OP_SHL:
            FOR_LANES(m) {
                uint32_t a = g->r[ra][l], s = gopnd_b(g, env, in, l);
                gset(g, d, l, bits(i, 39, 1) ? a << (s & 31) : (s > 31 ? 0 : a << s));
            }
            break;
        case OP_SHR: {
            int is_signed = bits(i, 48, 1), wrap = bits(i, 39, 1);
            FOR_LANES(m) {
                uint32_t a = g->r[ra][l], s = gopnd_b(g, env, in, l);
                if (wrap) {
                    s &= 31;
                } else if (s > 31) {
                    s = 31;
                    if (!is_signed) {
                        gset(g, d, l, 0);
                        continue;
                    }
                }
                gset(g, d, l, is_signed ? (uint32_t)((int32_t)a >> s) : a >> s);
            }
            break;
        }
        case OP_IADD:
            FOR_LANES(m) {
                uint32_t a = g->r[ra][l];
                uint32_t x = bits(i, 49, 1) ? 0u - a : a;
                uint32_t b = gopnd_b(g, env, in, l);
                b = bits(i, 48, 1) ? 0u - b : b;
                uint64_t sum = (uint64_t)x + b + (bits(i, 43, 1) ? (g->carry >> l) & 1 : 0);
                if (bits(i, 47, 1)) {
                    g->carry = (g->carry & ~(1u << l)) | ((uint32_t)((sum >> 32) & 1) << l);
                }
                gset(g, d, l, (uint32_t)sum);
            }
            break;
        case OP_ISCADD:
            FOR_LANES(m) {
                uint32_t a = g->r[ra][l];
                uint32_t x = (bits(i, 49, 1) ? 0u - a : a) << bits(i, 39, 5);
                uint32_t b = gopnd_b(g, env, in, l);
                gset(g, d, l, x + (bits(i, 48, 1) ? 0u - b : b));
            }
            break;
        case OP_LOP: {
            int i32 = is(i, 0x0400000000000000ull, 0xfc00000000000000ull);
            int nota = i32 ? bits(i, 55, 1) : bits(i, 39, 1);
            int notb = i32 ? bits(i, 56, 1) : bits(i, 40, 1);
            uint32_t op = i32 ? bits(i, 53, 2) : bits(i, 41, 2);
            FOR_LANES(m) {
                uint32_t a = g->r[ra][l], b = gopnd_b(g, env, in, l);
                uint32_t x = nota ? ~a : a;
                b = notb ? ~b : b;
                gset(g, d, l, op == 0 ? x & b : op == 1 ? x | b : op == 2 ? x ^ b : b);
            }
            break;
        }
        case OP_SEL:
            FOR_LANES(m) {
                int c = gpred(g, bits(i, 39, 3), l) ^ (int)bits(i, 42, 1);
                gset(g, d, l, c ? g->r[ra][l] : gopnd_b(g, env, in, l));
            }
            break;
        case OP_FMNMX:
            FOR_LANES(m) {
                float x = fmod_neg_abs(f(g->r[ra][l]), bits(i, 48, 1), bits(i, 46, 1));
                float y = fmod_neg_abs(f(gopnd_b(g, env, in, l)), bits(i, 45, 1), bits(i, 49, 1));
                int min = gpred(g, bits(i, 39, 3), l) ^ (int)bits(i, 42, 1);
                gset(g, d, l, u(min ? fminf(x, y) : fmaxf(x, y)));
            }
            break;
        case OP_FSETP:
            FOR_LANES(m) {
                float x = fmod_neg_abs(f(g->r[ra][l]), bits(i, 43, 1), bits(i, 7, 1));
                float y = fmod_neg_abs(f(gopnd_b(g, env, in, l)), bits(i, 6, 1), bits(i, 44, 1));
                int r = fcmp(bits(i, 48, 4), x, y);
                int c = gpred(g, bits(i, 39, 3), l) ^ (int)bits(i, 42, 1);
                uint32_t op = bits(i, 45, 2);
                gset_pred(g, bits(i, 3, 3), l, combine(op, r, c));
                gset_pred(g, bits(i, 0, 3), l, combine(op, !r, c));
            }
            break;
        case OP_ISETP:
            FOR_LANES(m) {
                int r = icmp(bits(i, 49, 3), g->r[ra][l], gopnd_b(g, env, in, l), bits(i, 48, 1));
                int c = gpred(g, bits(i, 39, 3), l) ^ (int)bits(i, 42, 1);
                uint32_t op = bits(i, 45, 2);
                gset_pred(g, bits(i, 3, 3), l, combine(op, r, c));
                gset_pred(g, bits(i, 0, 3), l, combine(op, !r, c));
            }
            break;
        case OP_TEXS: {
            uint32_t d2 = bits(i, 28, 8), sel = bits(i, 50, 3), rb = bits(i, 20, 8);
            tex_form form = texs_form(bits(i, 53, 4));
            if (form.coords == TEXC_UNSUPPORTED) {
                goto diverge; /* the scalar interpreter reports it */
            }
            const uint8_t *comp = d2 == RZ ? tex_one[sel] : (sel < 5 ? tex_two[sel] : tex_two[4]);
            uint32_t dst[4] = {d, d == RZ ? RZ : d + 1, d2, d2 == RZ ? RZ : d2 + 1};
            uint32_t ra1 = ra == RZ ? RZ : ra + 1;
            uint32_t ru = form.coords == TEXC_A1_B ? ra1 : ra;
            uint32_t rv = form.coords == TEXC_A_A1 ? ra1 : rb;
            shader_tex_coord tc[SHADER_LANES];
            quad_coords(g, m, ru, form.coords == TEXC_A ? RZ : rv, form.lod, tc);
            FOR_LANES(m) {
                if (form.lod == SHADER_LOD_EXPLICIT) {
                    tc[l].lod = f(g->r[rb][l]);
                }
                float texel[4] = {0, 0, 0, 0};
                if (env->tex) {
                    env->tex(env->tex_user, bits(i, 36, 13), &tc[l], texel);
                }
                for (int k = 0; k < 4 && comp[k] != 9; k++) {
                    gset(g, dst[k], l, u(texel[comp[k]]));
                }
            }
            break;
        }
        case OP_TLDS: {
            uint32_t d2 = bits(i, 28, 8), sel = bits(i, 50, 3), rb = bits(i, 20, 8);
            tex_form form = tlds_form(bits(i, 53, 4));
            if (form.coords == TEXC_UNSUPPORTED) {
                goto diverge; /* the scalar interpreter reports it */
            }
            const uint8_t *comp = d2 == RZ ? tex_one[sel] : (sel < 5 ? tex_two[sel] : tex_two[4]);
            uint32_t dst[4] = {d, d == RZ ? RZ : d + 1, d2, d2 == RZ ? RZ : d2 + 1};
            uint32_t ry = form.coords == TEXC_A_B ? rb : (ra == RZ ? RZ : ra + 1);
            FOR_LANES(m) {
                int32_t y = form.coords == TEXC_A ? 0 : (int32_t)g->r[ry][l];
                uint32_t level = form.lod == SHADER_LOD_EXPLICIT ? g->r[rb][l] : 0;
                float texel[4] = {0, 0, 0, 0};
                if (env->texel) {
                    env->texel(env->tex_user, bits(i, 36, 13), (int32_t)g->r[ra][l], y, level,
                               texel);
                }
                for (int k = 0; k < 4 && comp[k] != 9; k++) {
                    gset(g, dst[k], l, u(texel[comp[k]]));
                }
            }
            break;
        }
        case OP_TEX: {
            uint32_t mask = bits(i, 31, 4), rb = bits(i, 20, 8), rv = ra == RZ ? RZ : ra + 1;
            uint32_t lod_mode = tex_lod_mode(bits(i, 37, 3));
            shader_tex_coord tc[SHADER_LANES];
            quad_coords(g, m, ra, rv, (int)lod_mode, tc);
            FOR_LANES(m) {
                if (lod_mode == SHADER_LOD_EXPLICIT || lod_mode == SHADER_LOD_BIAS) {
                    tc[l].lod = f(g->r[rb][l]);
                }
                float texel[4] = {0, 0, 0, 0};
                if (env->tex) {
                    env->tex(env->tex_user, bits(i, 36, 13), &tc[l], texel);
                }
                uint32_t k = 0;
                for (int c = 0; c < 4; c++) {
                    if (mask & (1u << c)) {
                        gset(g, d == RZ ? RZ : d + k++, l, u(texel[c]));
                    }
                }
            }
            break;
        }
        default:
            goto diverge; /* attributes, compute and memory ops: one lane at a time */
        }
        pc = next;
        continue;
    diverge:
        return run_lanes(prog, g, live, env, pc, &cf, bad_insn);
    }
    *bad_insn = 0;
    return -1;
}
