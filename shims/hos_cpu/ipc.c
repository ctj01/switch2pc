/*
 * IPC transport: sessions, domains, CMIF/TIPC framing and the service
 * registry. One lock serialises every request; services are simple enough
 * that this costs nothing yet.
 */

#include <inttypes.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>

#include "common/hos_log.h"
#include "cpu_internal.h"
#include "hipc.h"

#define MOD "ipc"

#define SERVICE_MAX     64
#define OBJECT_POOL     4096
#define SESSION_POOL    1024
#define DOMAIN_MAX_OBJS 64
#define TLS_IPC_WORDS   (HOS_TLS_IPC_SIZE / 4)

/* Control commands (type 5). */
enum {
    CTRL_CONVERT_TO_DOMAIN = 0,
    CTRL_COPY_FROM_DOMAIN = 1,
    CTRL_CLONE = 2,
    CTRL_QUERY_POINTER_BUFFER_SIZE = 3,
    CTRL_CLONE_EX = 4,
};

#define POINTER_BUFFER_SIZE 0x8000

struct hos_session {
    hos_object *root;
    int is_domain;
    hos_object *objs[DOMAIN_MAX_OBJS]; /* domain object id = index + 1 */
    struct hos_session *next_free;
};

typedef struct service_entry {
    char name[9];
    const hos_service_class *cls;
    void *ctx;
} service_entry;

typedef struct domain_in_header {
    uint8_t cmd;
    uint8_t num_in_objects;
    uint16_t data_payload_size;
    uint32_t object_id;
    uint32_t pad;
    uint32_t token;
} domain_in_header;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;      /* requests and registry */
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER; /* object and session pools */
static service_entry g_services[SERVICE_MAX];
static size_t g_num_services;

static hos_object *g_object_pool;
static hos_object *g_object_free;
static hos_session *g_session_pool;
static hos_session *g_session_free;

static void *pool_alloc(size_t bytes) {
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

hos_result hos_ipc_init(void) {
    g_object_pool = pool_alloc(sizeof(hos_object) * OBJECT_POOL);
    g_session_pool = pool_alloc(sizeof(hos_session) * SESSION_POOL);
    if (!g_object_pool || !g_session_pool) {
        return HOS_KERNEL_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < OBJECT_POOL; i++) {
        /* reuse ctx as the free-list link */
        g_object_pool[i].ctx = (i + 1 < OBJECT_POOL) ? &g_object_pool[i + 1] : NULL;
    }
    g_object_free = &g_object_pool[0];
    for (size_t i = 0; i < SESSION_POOL; i++) {
        g_session_pool[i].next_free = (i + 1 < SESSION_POOL) ? &g_session_pool[i + 1] : NULL;
    }
    g_session_free = &g_session_pool[0];
    hos_sm_init();
    return HOS_RESULT_SUCCESS;
}

/* ---- registry ------------------------------------------------------------ */

hos_result hos_ipc_register_service(const char *name, const hos_service_class *cls, void *ctx) {
    if (!name || strlen(name) > 8 || !cls || !cls->dispatch) {
        return HOS_KERNEL_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_num_services; i++) {
        if (strcmp(g_services[i].name, name) == 0) {
            pthread_mutex_unlock(&g_lock);
            HOS_LOGW(MOD, "service %s registered twice", name);
            return HOS_KERNEL_BUSY;
        }
    }
    if (g_num_services >= SERVICE_MAX) {
        pthread_mutex_unlock(&g_lock);
        return HOS_KERNEL_OUT_OF_RESOURCE;
    }
    service_entry *e = &g_services[g_num_services++];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, 8);
    e->cls = cls;
    e->ctx = ctx;
    pthread_mutex_unlock(&g_lock);
    HOS_LOGD(MOD, "service %s registered", name);
    return HOS_RESULT_SUCCESS;
}

static const service_entry *find_service(const char *name) {
    for (size_t i = 0; i < g_num_services; i++) {
        if (strcmp(g_services[i].name, name) == 0) {
            return &g_services[i];
        }
    }
    return NULL;
}

/* Called by sm with the lock held. */
HOS_HIDDEN hos_object *hos_ipc_service_instance(const char *name) {
    const service_entry *e = find_service(name);
    if (!e) {
        return NULL;
    }
    return hos_ipc_object_new(e->cls, e->ctx);
}

/* ---- objects and sessions -------------------------------------------------- */

hos_object *hos_ipc_object_new(const hos_service_class *cls, void *ctx) {
    pthread_mutex_lock(&g_pool_lock);
    hos_object *o = g_object_free;
    if (o) {
        g_object_free = o->ctx;
    }
    pthread_mutex_unlock(&g_pool_lock);
    if (!o) {
        HOS_LOGE(MOD, "object pool exhausted");
        return NULL;
    }
    o->cls = cls;
    o->ctx = ctx;
    o->refs = 1;
    return o;
}

static void object_ref(hos_object *o) {
    __atomic_add_fetch(&o->refs, 1, __ATOMIC_SEQ_CST);
}

void hos_ipc_object_unref(hos_object *o) {
    if (!o) {
        return;
    }
    if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_SEQ_CST) != 0) {
        return;
    }
    if (o->cls->destroy) {
        o->cls->destroy(o);
    }
    pthread_mutex_lock(&g_pool_lock);
    o->cls = NULL;
    o->ctx = g_object_free;
    g_object_free = o;
    pthread_mutex_unlock(&g_pool_lock);
}

static hos_session *session_new(hos_object *root) {
    pthread_mutex_lock(&g_pool_lock);
    hos_session *s = g_session_free;
    if (s) {
        g_session_free = s->next_free;
    }
    pthread_mutex_unlock(&g_pool_lock);
    if (!s) {
        HOS_LOGE(MOD, "session pool exhausted");
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    s->root = root;
    return s;
}

uint32_t hos_ipc_session_create(hos_object *obj) {
    hos_session *s = session_new(obj);
    if (!s) {
        hos_ipc_object_unref(obj);
        return 0;
    }
    uint32_t h = hos_handle_alloc(HOS_OBJ_SESSION, s);
    if (!h) {
        hos_session_close(s);
    }
    return h;
}

void hos_session_close(hos_session *s) {
    if (s->is_domain) {
        for (int i = 0; i < DOMAIN_MAX_OBJS; i++) {
            if (s->objs[i]) {
                hos_ipc_object_unref(s->objs[i]);
                s->objs[i] = NULL;
            }
        }
    }
    hos_ipc_object_unref(s->root);
    pthread_mutex_lock(&g_pool_lock);
    s->root = NULL;
    s->next_free = g_session_free;
    g_session_free = s;
    pthread_mutex_unlock(&g_pool_lock);
}

static int domain_add(hos_session *s, hos_object *o) {
    for (int i = 0; i < DOMAIN_MAX_OBJS; i++) {
        if (!s->objs[i]) {
            s->objs[i] = o;
            return i + 1;
        }
    }
    return 0;
}

static hos_object *domain_get(hos_session *s, uint32_t id) {
    if (id == 0 || id > DOMAIN_MAX_OBJS) {
        return NULL;
    }
    return s->objs[id - 1];
}

/* ---- helpers for service code ---------------------------------------------- */

int hos_ipc_in_read(const hos_ipc_ctx *ctx, size_t off, void *dst, size_t n) {
    if (off + n > ctx->in_size) {
        return 0;
    }
    memcpy(dst, ctx->in + off, n);
    return 1;
}

void hos_ipc_out_write(hos_ipc_ctx *ctx, const void *src, size_t n) {
    if (ctx->out_size + n > HOS_IPC_OUT_MAX) {
        n = HOS_IPC_OUT_MAX - ctx->out_size;
    }
    memcpy(ctx->out + ctx->out_size, src, n);
    ctx->out_size += n;
}

hos_result hos_ipc_return_object(hos_ipc_ctx *ctx, hos_object *obj) {
    if (!obj) {
        return HOS_KERNEL_OUT_OF_RESOURCE;
    }
    if (ctx->num_out_objects >= HOS_IPC_MAX_OBJECTS) {
        hos_ipc_object_unref(obj);
        return HOS_KERNEL_OUT_OF_RANGE;
    }
    ctx->out_objects[ctx->num_out_objects++] = obj;
    return HOS_RESULT_SUCCESS;
}

hos_result hos_ipc_unimplemented(const hos_object *obj, const hos_ipc_ctx *ctx) {
    HOS_LOGW(MOD, "%s: command %u not implemented (in %zu bytes, %u buffers)", obj->cls->name,
             ctx->cmd, ctx->in_size, ctx->num_a + ctx->num_b + ctx->num_x + ctx->num_c);
    return HOS_KERNEL_NOT_IMPLEMENTED;
}

/* ---- request handling ------------------------------------------------------- */

static void ctx_from_msg(hos_ipc_ctx *ctx, const hipc_msg *m) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->num_a = m->num_a;
    ctx->num_b = m->num_b;
    ctx->num_x = m->num_x;
    ctx->num_c = m->num_c;
    memcpy(ctx->a, m->a, sizeof(ctx->a));
    memcpy(ctx->b, m->b, sizeof(ctx->b));
    memcpy(ctx->x, m->x, sizeof(ctx->x));
    memcpy(ctx->c, m->c, sizeof(ctx->c));
    ctx->num_in_copy = m->num_copy;
    ctx->num_in_move = m->num_move;
    memcpy(ctx->in_copy_handles, m->copy, sizeof(ctx->in_copy_handles));
    memcpy(ctx->in_move_handles, m->move, sizeof(ctx->in_move_handles));
    ctx->has_pid = m->has_pid;
    ctx->pid = m->pid;
}

/* Finish a CMIF response: out objects become ids (domain) or move handles. */
static hos_result write_cmif_reply(hos_session *s, uint32_t *buf, hos_ipc_ctx *ctx,
                                   hos_result result, int domain_reply) {
    uint32_t out_words = (uint32_t)((ctx->out_size + 3) / 4);
    uint32_t ids[HOS_IPC_MAX_OBJECTS];
    uint32_t num_ids = 0;

    for (uint32_t i = 0; i < ctx->num_out_objects; i++) {
        hos_object *o = ctx->out_objects[i];
        if (domain_reply) {
            int id = domain_add(s, o);
            if (!id) {
                hos_ipc_object_unref(o);
                HOS_LOGE(MOD, "domain full");
                continue;
            }
            ids[num_ids++] = (uint32_t)id;
        } else {
            uint32_t h = hos_ipc_session_create(o);
            if (h && ctx->num_move < HOS_IPC_MAX_HANDLES) {
                ctx->move_handles[ctx->num_move++] = h;
            }
        }
    }

    uint32_t payload_words = (domain_reply ? 4 : 0) + 4 + out_words + num_ids;
    uint32_t *p = hipc_write_header(buf, TLS_IPC_WORDS, 0, ctx->copy_handles, ctx->num_copy,
                                    ctx->move_handles, ctx->num_move, payload_words, 1);
    if (!p) {
        HOS_LOGE(MOD, "response does not fit in the TLS buffer");
        return HOS_KERNEL_OUT_OF_RANGE;
    }
    if (domain_reply) {
        p[0] = num_ids;
        p += 4;
    }
    p[0] = CMIF_MAGIC_SFCO;
    p[1] = 0;
    p[2] = result;
    p[3] = 0;
    memcpy(p + 4, ctx->out, ctx->out_size);
    memcpy(p + 4 + out_words, ids, num_ids * 4);
    return HOS_RESULT_SUCCESS;
}

static hos_result write_tipc_reply(uint32_t *buf, hos_ipc_ctx *ctx, hos_result result) {
    for (uint32_t i = 0; i < ctx->num_out_objects; i++) {
        uint32_t h = hos_ipc_session_create(ctx->out_objects[i]);
        if (h && ctx->num_move < HOS_IPC_MAX_HANDLES) {
            ctx->move_handles[ctx->num_move++] = h;
        }
    }
    uint32_t out_words = (uint32_t)((ctx->out_size + 3) / 4);
    uint32_t *p = hipc_write_header(buf, TLS_IPC_WORDS, 0, ctx->copy_handles, ctx->num_copy,
                                    ctx->move_handles, ctx->num_move, 1 + out_words, 0);
    if (!p) {
        return HOS_KERNEL_OUT_OF_RANGE;
    }
    p[0] = result;
    memcpy(p + 1, ctx->out, ctx->out_size);
    return HOS_RESULT_SUCCESS;
}

static hos_result handle_control(hos_session *s, uint32_t *buf, hipc_msg *m) {
    if (m->data_words < 4 || m->data[0] != CMIF_MAGIC_SFCI) {
        return HOS_KERNEL_INVALID_COMBINATION;
    }
    hos_ipc_ctx ctx;
    ctx_from_msg(&ctx, m);
    ctx.cmd = m->data[2];
    ctx.in = (const uint8_t *)(m->data + 4);
    ctx.in_size = (m->data_words - 4) * 4;
    hos_result res = HOS_RESULT_SUCCESS;

    switch (ctx.cmd) {
    case CTRL_CONVERT_TO_DOMAIN: {
        if (s->is_domain) {
            res = HOS_KERNEL_INVALID_STATE;
            break;
        }
        s->is_domain = 1;
        object_ref(s->root);
        uint32_t id = (uint32_t)domain_add(s, s->root);
        hos_ipc_out_write(&ctx, &id, 4);
        HOS_LOGD(MOD, "%s: converted to domain", s->root->cls->name);
        break;
    }
    case CTRL_COPY_FROM_DOMAIN: {
        uint32_t id = 0;
        hos_ipc_in_read(&ctx, 0, &id, 4);
        hos_object *o = s->is_domain ? domain_get(s, id) : NULL;
        if (!o) {
            res = HOS_KERNEL_NOT_FOUND;
            break;
        }
        object_ref(o);
        uint32_t h = hos_ipc_session_create(o);
        ctx.move_handles[ctx.num_move++] = h;
        break;
    }
    case CTRL_CLONE:
    case CTRL_CLONE_EX: {
        object_ref(s->root);
        uint32_t h = hos_ipc_session_create(s->root);
        ctx.move_handles[ctx.num_move++] = h;
        break;
    }
    case CTRL_QUERY_POINTER_BUFFER_SIZE: {
        uint32_t size = POINTER_BUFFER_SIZE;
        hos_ipc_out_write(&ctx, &size, 4);
        break;
    }
    default:
        HOS_LOGW(MOD, "unknown control command %u", ctx.cmd);
        res = HOS_KERNEL_NOT_IMPLEMENTED;
        break;
    }
    return write_cmif_reply(s, buf, &ctx, res, 0);
}

static hos_result handle_request(hos_session *s, uint32_t *buf, hipc_msg *m) {
    hos_ipc_ctx ctx;
    ctx_from_msg(&ctx, m);
    uint32_t *p = m->data;
    uint32_t words = m->data_words;
    hos_object *target = s->root;
    int domain_reply = 0;

    if (s->is_domain) {
        if (words < 4) {
            return HOS_KERNEL_INVALID_COMBINATION;
        }
        domain_in_header dh;
        memcpy(&dh, p, sizeof(dh));
        p += 4;
        words -= 4;
        domain_reply = 1;
        if (dh.cmd == 2) {
            /* close a domain object */
            hos_object *o = domain_get(s, dh.object_id);
            if (o) {
                s->objs[dh.object_id - 1] = NULL;
                hos_ipc_object_unref(o);
            }
            ctx.out_size = 0;
            return write_cmif_reply(s, buf, &ctx, HOS_RESULT_SUCCESS, 1);
        }
        target = domain_get(s, dh.object_id);
        if (!target) {
            HOS_LOGW(MOD, "domain object %u does not exist", dh.object_id);
            return write_cmif_reply(s, buf, &ctx, HOS_KERNEL_NOT_FOUND, 1);
        }
        uint32_t payload_words = dh.data_payload_size / 4;
        if (payload_words > words) {
            payload_words = words;
        }
        const uint32_t *objs = p + payload_words;
        ctx.num_in_objects = dh.num_in_objects;
        if (ctx.num_in_objects > HOS_IPC_MAX_OBJECTS) {
            ctx.num_in_objects = HOS_IPC_MAX_OBJECTS;
        }
        for (uint32_t i = 0; i < ctx.num_in_objects && payload_words + i < words; i++) {
            ctx.in_objects[i] = domain_get(s, objs[i]);
        }
        words = payload_words;
    }

    if (words < 4 || p[0] != CMIF_MAGIC_SFCI) {
        HOS_LOGW(MOD, "%s: request without SFCI header", s->root->cls->name);
        return HOS_KERNEL_INVALID_COMBINATION;
    }
    ctx.cmd = p[2];
    ctx.in = (const uint8_t *)(p + 4);
    ctx.in_size = (words - 4) * 4;

    HOS_LOGT(MOD, "%s cmd %u (%zu bytes in)", target->cls->name, ctx.cmd, ctx.in_size);
    hos_result res = target->cls->dispatch(target, &ctx);
    return write_cmif_reply(s, buf, &ctx, res, domain_reply);
}

static hos_result handle_tipc(hos_session *s, uint32_t *buf, hipc_msg *m) {
    hos_ipc_ctx ctx;
    ctx_from_msg(&ctx, m);
    ctx.tipc = 1;
    ctx.cmd = m->type - HIPC_TYPE_TIPC_BASE;
    ctx.in = (const uint8_t *)m->raw;
    ctx.in_size = m->raw_words * 4;
    HOS_LOGT(MOD, "%s tipc cmd %u (%zu bytes in)", s->root->cls->name, ctx.cmd, ctx.in_size);
    hos_result res = s->root->cls->dispatch(s->root, &ctx);
    return write_tipc_reply(buf, &ctx, res);
}

hos_result hos_ipc_send_sync(hos_session *s, uint32_t *tls_buf) {
    uint32_t msg[TLS_IPC_WORDS];
    memcpy(msg, tls_buf, sizeof(msg));
    hipc_msg m;
    hos_result r = hipc_parse(msg, TLS_IPC_WORDS, &m);
    if (HOS_FAILED(r)) {
        HOS_LOGW(MOD, "malformed message (type %#x)", msg[0] & 0xFFFF);
        return r;
    }

    pthread_mutex_lock(&g_lock);
    switch (m.type) {
    case HIPC_TYPE_CLOSE:
        /* The handle stays valid until svcCloseHandle; HOS reports SessionClosed. */
        r = HOS_KERNEL_SESSION_CLOSED;
        break;
    case HIPC_TYPE_REQUEST:
    case HIPC_TYPE_REQUEST_WITH_CONTEXT:
        r = handle_request(s, tls_buf, &m);
        break;
    case HIPC_TYPE_CONTROL:
    case HIPC_TYPE_CONTROL_WITH_CONTEXT:
        r = handle_control(s, tls_buf, &m);
        break;
    default:
        if (m.type >= HIPC_TYPE_TIPC_BASE) {
            r = handle_tipc(s, tls_buf, &m);
        } else {
            HOS_LOGW(MOD, "unsupported message type %u", m.type);
            r = HOS_KERNEL_NOT_IMPLEMENTED;
        }
        break;
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

void hos_ipc_blocking_begin(void) {
    pthread_mutex_unlock(&g_lock);
}

void hos_ipc_blocking_end(void) {
    pthread_mutex_lock(&g_lock);
}

/* ---- svc entry points ------------------------------------------------------- */

hos_result hos_ipc_connect_named_port(const char *name, uint32_t *out_handle) {
    pthread_mutex_lock(&g_lock);
    hos_object *o = hos_ipc_service_instance(name);
    pthread_mutex_unlock(&g_lock);
    if (!o) {
        HOS_LOGW(MOD, "ConnectToNamedPort(%s): no such port", name);
        return HOS_KERNEL_NOT_FOUND;
    }
    uint32_t h = hos_ipc_session_create(o);
    if (!h) {
        return HOS_KERNEL_OUT_OF_RESOURCE;
    }
    *out_handle = h;
    HOS_LOGD(MOD, "ConnectToNamedPort(%s) -> %#x", name, h);
    return HOS_RESULT_SUCCESS;
}

void svc_connect_to_named_port(hos_svc_regs *r) {
    const char *p = (const char *)r->x[1];
    char name[12];
    if (!p) {
        r->x[0] = HOS_KERNEL_INVALID_POINTER;
        return;
    }
    size_t n = strnlen(p, sizeof(name));
    if (n >= sizeof(name)) {
        r->x[0] = HOS_KERNEL_OUT_OF_RANGE;
        return;
    }
    memcpy(name, p, n);
    name[n] = 0;
    uint32_t h = 0;
    r->x[0] = hos_ipc_connect_named_port(name, &h);
    r->x[1] = h;
}

void svc_send_sync_request(hos_svc_regs *r) {
    hos_session *s = hos_handle_get((uint32_t)r->x[0], HOS_OBJ_SESSION);
    if (!s) {
        r->x[0] = HOS_KERNEL_INVALID_HANDLE;
        return;
    }
    if (!hos_current_thread) {
        r->x[0] = HOS_KERNEL_INVALID_STATE;
        return;
    }
    r->x[0] = hos_ipc_send_sync(s, (uint32_t *)(hos_current_thread->tls + HOS_TLS_IPC_BUFFER));
}
