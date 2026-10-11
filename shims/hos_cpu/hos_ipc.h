#ifndef HOS_IPC_H
#define HOS_IPC_H

/*
 * IPC for service libraries. libhos_cpu owns the transport: sessions, HIPC
 * parsing, CMIF/TIPC framing, domains and the `sm:` port. A service library
 * (libhos_fs, libhos_os, ...) registers a class per service name from a
 * constructor; every `GetService` from the game then creates one object of
 * that class behind a fresh session handle and routes requests to its
 * dispatch function.
 *
 * A dispatch function reads `ctx->cmd` and `ctx->in`, writes `ctx->out`, and
 * returns the HOS result the game should see in the response header. Buffers
 * passed by the game are listed in a/b/x/c with their guest addresses, which
 * are plain host pointers in this process.
 */

#include <stddef.h>
#include <stdint.h>

#include "common/hos_result.h"
#include "hos_cpu.h"

#define HOS_IPC_MAX_BUFFERS 8
#define HOS_IPC_MAX_HANDLES 8
#define HOS_IPC_MAX_OBJECTS 8
#define HOS_IPC_OUT_MAX     0x100

typedef struct hos_object hos_object;
typedef struct hos_session hos_session;
typedef struct hos_ipc_ctx hos_ipc_ctx;

typedef struct hos_ipc_buffer {
    uint64_t addr;
    uint64_t size;
} hos_ipc_buffer;

struct hos_ipc_ctx {
    uint32_t cmd;
    int tipc; /* request arrived as TIPC (no CMIF header) */

    const uint8_t *in; /* payload after the CMIF/TIPC header */
    size_t in_size;

    hos_ipc_buffer a[HOS_IPC_MAX_BUFFERS]; /* send buffers (game -> service) */
    hos_ipc_buffer b[HOS_IPC_MAX_BUFFERS]; /* receive buffers (service -> game) */
    hos_ipc_buffer x[HOS_IPC_MAX_BUFFERS]; /* pointer send buffers */
    hos_ipc_buffer c[HOS_IPC_MAX_BUFFERS]; /* pointer receive buffers */
    uint32_t num_a, num_b, num_x, num_c;

    uint32_t in_copy_handles[HOS_IPC_MAX_HANDLES];
    uint32_t in_move_handles[HOS_IPC_MAX_HANDLES];
    uint32_t num_in_copy, num_in_move;
    int has_pid;
    uint64_t pid;

    hos_object *in_objects[HOS_IPC_MAX_OBJECTS]; /* domain in-objects */
    uint32_t num_in_objects;

    /* Outputs, filled by the dispatch function. */
    uint8_t out[HOS_IPC_OUT_MAX];
    size_t out_size;
    uint32_t copy_handles[HOS_IPC_MAX_HANDLES];
    uint32_t move_handles[HOS_IPC_MAX_HANDLES];
    uint32_t num_copy, num_move;
    hos_object *out_objects[HOS_IPC_MAX_OBJECTS]; /* interfaces returned to the game */
    uint32_t num_out_objects;
};

typedef struct hos_service_class {
    const char *name;
    hos_result (*dispatch)(hos_object *obj, hos_ipc_ctx *ctx);
    void (*destroy)(hos_object *obj); /* may be NULL */
} hos_service_class;

struct hos_object {
    const hos_service_class *cls;
    void *ctx; /* per-object state owned by the class */
    int refs;
};

/**
 * Register a service under its `sm:` name ("fsp-srv", "lm", ...). `ctx` is
 * passed to every object created for it. Call from a library constructor.
 *
 * @return HOS_RESULT_SUCCESS, HOS_KERNEL_OUT_OF_RESOURCE or HOS_KERNEL_BUSY if taken
 */
HOS_EXPORT hos_result hos_ipc_register_service(const char *name, const hos_service_class *cls,
                                               void *ctx);

/**
 * Allocate an object with one reference owned by the caller.
 */
HOS_EXPORT hos_object *hos_ipc_object_new(const hos_service_class *cls, void *ctx);

/**
 * Drop a reference; destroys the object when the last one goes.
 */
HOS_EXPORT void hos_ipc_object_unref(hos_object *obj);

/**
 * Return an interface to the game. Takes ownership of the reference. On a
 * domain session it becomes an object id, otherwise a move handle to a new
 * session.
 */
HOS_EXPORT hos_result hos_ipc_return_object(hos_ipc_ctx *ctx, hos_object *obj);

/**
 * Wrap an object in a new session and return its handle. Takes ownership of
 * the reference. 0 on failure.
 */
HOS_EXPORT uint32_t hos_ipc_session_create(hos_object *obj);

/**
 * Copy `n` bytes of request payload at `off` into `dst`. Returns 0 if the
 * request is shorter than that.
 */
HOS_EXPORT int hos_ipc_in_read(const hos_ipc_ctx *ctx, size_t off, void *dst, size_t n);

/**
 * Append `n` bytes to the response payload. Silently truncates at HOS_IPC_OUT_MAX.
 */
HOS_EXPORT void hos_ipc_out_write(hos_ipc_ctx *ctx, const void *src, size_t n);

/**
 * Log an unimplemented command at WARN and return HOS_KERNEL_NOT_IMPLEMENTED.
 */
HOS_EXPORT hos_result hos_ipc_unimplemented(const hos_object *obj, const hos_ipc_ctx *ctx);

/**
 * Create a HOS event for a service. Returns the readable handle to hand to the
 * game as a copy handle (0 on failure); `*ev` is the token for the two calls
 * below. The event lives for the life of the process.
 */
HOS_EXPORT uint32_t hos_ipc_event_create(void **ev);

/**
 * Signal / clear an event created with hos_ipc_event_create.
 */
HOS_EXPORT void hos_ipc_event_signal(void *ev);
HOS_EXPORT void hos_ipc_event_clear(void *ev);

/*
 * Ask the application to leave the way the HOME menu does: the applet shim
 * delivers AppletMessage_ExitRequest on the next ReceiveMessage, libnx's
 * appletMainLoop returns false and the game exits on its own. Without an
 * installed handler (no applet shim linked) the process exits at once.
 */
/*
 * Inside a dispatch function, around a host call that may block for long
 * (socket recv/accept/connect/poll): drops the IPC request lock so other game
 * threads keep talking to services meanwhile. Between the two calls the
 * handler must touch only its own ctx and state guarded by its own lock.
 */
HOS_EXPORT void hos_ipc_blocking_begin(void);
HOS_EXPORT void hos_ipc_blocking_end(void);

HOS_EXPORT void hos_applet_request_exit(void);
HOS_EXPORT void hos_applet_set_exit_handler(void (*fn)(void));

/*
 * An external pad: a front end that owns the keyboard (the X11 window) sets
 * player 1's state here, and the hid writer thread takes it over the scripted
 * or file sources while it is active. Buttons use the Switch bit numbering,
 * sticks are -32767..32767.
 */
HOS_EXPORT void hos_pad_external_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx,
                                     int32_t ry);
/* Returns 0 until hos_pad_external_set has been called once, then 1 and the state. */
HOS_EXPORT int hos_pad_external_get(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx,
                                    int32_t *ry);

/**
 * Copy a service-owned handle for the game: adds a copy-handle entry to the
 * reply that refers to the same object. Returns 0 on failure.
 */
HOS_EXPORT uint32_t hos_ipc_copy_handle(uint32_t handle);

#endif /* HOS_IPC_H */
