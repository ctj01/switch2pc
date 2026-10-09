/*
 * fsp-srv: the filesystem service. Mount points are host directories:
 *
 *   HOS_SDMC   directory served as the SD card (OpenSdCardFileSystem)
 *   HOS_SAVE   directory served as save data (OpenSaveDataFileSystem)
 *   HOS_ROMFS  file served as the data storage (OpenDataStorageByCurrentProcess),
 *              a romfs image as hactool or build_romfs produce it
 *
 * hosrun sets these from --sdmc / --save / --romfs. Unset mounts report the
 * same results the real service gives for a missing card or title.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "common/hos_log.h"
#include "hos_ipc.h"
#include "romfs_build.h"

#define MOD "fs"

#define HOS_MODULE_FS            2
#define FS_PATH_NOT_FOUND        HOS_RESULT_MAKE(HOS_MODULE_FS, 1)
#define FS_PATH_ALREADY_EXISTS   HOS_RESULT_MAKE(HOS_MODULE_FS, 2)
#define FS_TARGET_LOCKED         HOS_RESULT_MAKE(HOS_MODULE_FS, 7)
#define FS_DIRECTORY_NOT_EMPTY   HOS_RESULT_MAKE(HOS_MODULE_FS, 8)
#define FS_TARGET_NOT_FOUND      HOS_RESULT_MAKE(HOS_MODULE_FS, 1002)
#define FS_SD_CARD_NOT_PRESENT   HOS_RESULT_MAKE(HOS_MODULE_FS, 2520)
#define FS_NOT_IMPLEMENTED       HOS_RESULT_MAKE(HOS_MODULE_FS, 3001)
#define FS_OUT_OF_RANGE          HOS_RESULT_MAKE(HOS_MODULE_FS, 3005)
#define FS_INVALID_ARGUMENT      HOS_RESULT_MAKE(HOS_MODULE_FS, 6001)
#define FS_INVALID_PATH          HOS_RESULT_MAKE(HOS_MODULE_FS, 6002)
#define FS_UNSUPPORTED_OPERATION HOS_RESULT_MAKE(HOS_MODULE_FS, 6300)
#define FS_WRITE_NOT_PERMITTED   HOS_RESULT_MAKE(HOS_MODULE_FS, 6203)

#define FS_PATH_MAX   0x301
#define FS_ENTRY_SIZE 0x310
#define POOL_FS       16
#define POOL_FILES    256
#define POOL_DIRS     64
#define POOL_STORAGES 16

enum open_mode { OPEN_READ = 1, OPEN_WRITE = 2, OPEN_APPEND = 4 };
enum dir_mode { DIR_DIRECTORIES = 1, DIR_FILES = 2 };
enum entry_type { ENTRY_DIRECTORY = 0, ENTRY_FILE = 1 };

typedef struct fs_mount {
    int used;
    int read_only;
    char root[PATH_MAX];
} fs_mount;

typedef struct fs_file {
    int used;
    int fd;
    uint32_t mode;
} fs_file;

typedef struct fs_dir {
    int used;
    DIR *dir;
    uint32_t mode;
    char path[PATH_MAX];
} fs_dir;

typedef struct fs_storage {
    int used;
    int fd;           /* image file, or -1 */
    romfs_image *img; /* image built from a directory (shared, never freed) */
    int read_only;
} fs_storage;

static romfs_image *g_romfs_dir_image;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static fs_mount g_mounts[POOL_FS];
static fs_file g_files[POOL_FILES];
static fs_dir g_dirs[POOL_DIRS];
static fs_storage g_storages[POOL_STORAGES];

#define POOL_TAKE(pool, count, out)                                                                \
    do {                                                                                           \
        (out) = NULL;                                                                              \
        pthread_mutex_lock(&g_lock);                                                               \
        for (int i_ = 0; i_ < (count); i_++) {                                                     \
            if (!(pool)[i_].used) {                                                                \
                memset(&(pool)[i_], 0, sizeof((pool)[i_]));                                        \
                (pool)[i_].used = 1;                                                               \
                (out) = &(pool)[i_];                                                               \
                break;                                                                             \
            }                                                                                      \
        }                                                                                          \
        pthread_mutex_unlock(&g_lock);                                                             \
    } while (0)

static hos_result errno_result(int e) {
    switch (e) {
    case ENOENT:
    case ENOTDIR:
        return FS_PATH_NOT_FOUND;
    case EEXIST:
        return FS_PATH_ALREADY_EXISTS;
    case ENOTEMPTY:
        return FS_DIRECTORY_NOT_EMPTY;
    case EACCES:
    case EPERM:
    case EROFS:
        return FS_WRITE_NOT_PERMITTED;
    case EBUSY:
        return FS_TARGET_LOCKED;
    default:
        return FS_INVALID_ARGUMENT;
    }
}

/* Join root and a guest path, refusing anything that escapes the root. */
static hos_result resolve(const fs_mount *m, const hos_ipc_ctx *ctx, char *out, size_t cap) {
    if (ctx->num_x == 0 || !ctx->x[0].addr) {
        return FS_INVALID_PATH;
    }
    const char *guest = (const char *)ctx->x[0].addr;
    size_t glen = strnlen(guest, FS_PATH_MAX);
    if (glen >= FS_PATH_MAX) {
        return FS_INVALID_PATH;
    }
    /* strip a mount prefix like "sdmc:" and leading slashes */
    const char *colon = memchr(guest, ':', glen);
    if (colon) {
        guest = colon + 1;
    }
    while (*guest == '/') {
        guest++;
    }
    /* reject ".." components */
    const char *p = guest;
    while (*p) {
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == 2 && p[0] == '.' && p[1] == '.') {
            return FS_INVALID_PATH;
        }
        p += n;
        while (*p == '/') {
            p++;
        }
    }
    int w = snprintf(out, cap, "%s/%s", m->root, guest);
    if (w < 0 || (size_t)w >= cap) {
        return FS_INVALID_PATH;
    }
    /* drop a trailing slash so stat works on directories too */
    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '/') {
        out[--len] = '\0';
    }
    return HOS_RESULT_SUCCESS;
}

/* Second X buffer: destination path for rename. */
static hos_result resolve_second(const fs_mount *m, const hos_ipc_ctx *ctx, char *out, size_t cap) {
    hos_ipc_ctx tmp = *ctx;
    if (ctx->num_x < 2) {
        return FS_INVALID_PATH;
    }
    tmp.x[0] = ctx->x[1];
    return resolve(m, &tmp, out, cap);
}

/* ---- IFile ----------------------------------------------------------------------- */

typedef struct file_io_args {
    uint32_t option;
    uint32_t pad;
    int64_t offset;
    int64_t size;
} file_io_args;

static hos_result file_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    fs_file *f = obj->ctx;
    switch (ctx->cmd) {
    case 0: /* Read(option, offset, size, out buffer) -> bytes read */ {
        file_io_args a;
        if (!hos_ipc_in_read(ctx, 0, &a, sizeof(a)) || ctx->num_b == 0) {
            return FS_INVALID_ARGUMENT;
        }
        if (a.offset < 0 || a.size < 0) {
            return FS_OUT_OF_RANGE;
        }
        uint64_t n = (uint64_t)a.size < ctx->b[0].size ? (uint64_t)a.size : ctx->b[0].size;
        ssize_t got = pread(f->fd, (void *)ctx->b[0].addr, n, (off_t)a.offset);
        if (got < 0) {
            return errno_result(errno);
        }
        int64_t out = got;
        hos_ipc_out_write(ctx, &out, 8);
        return HOS_RESULT_SUCCESS;
    }
    case 1: /* Write(option, offset, size, in buffer) */ {
        file_io_args a;
        if (!hos_ipc_in_read(ctx, 0, &a, sizeof(a)) || ctx->num_a == 0) {
            return FS_INVALID_ARGUMENT;
        }
        if (!(f->mode & OPEN_WRITE)) {
            return FS_WRITE_NOT_PERMITTED;
        }
        uint64_t n = (uint64_t)a.size < ctx->a[0].size ? (uint64_t)a.size : ctx->a[0].size;
        const uint8_t *p = (const uint8_t *)ctx->a[0].addr;
        off_t off = (off_t)a.offset;
        while (n) {
            ssize_t w = pwrite(f->fd, p, n, off);
            if (w < 0) {
                return errno_result(errno);
            }
            p += w;
            off += w;
            n -= (uint64_t)w;
        }
        if (a.option & 1) { /* WriteOption::Flush */
            fsync(f->fd);
        }
        return HOS_RESULT_SUCCESS;
    }
    case 2: /* Flush */
        fsync(f->fd);
        return HOS_RESULT_SUCCESS;
    case 3: /* SetSize(s64) */ {
        int64_t size = 0;
        hos_ipc_in_read(ctx, 0, &size, 8);
        if (!(f->mode & OPEN_WRITE)) {
            return FS_WRITE_NOT_PERMITTED;
        }
        if (ftruncate(f->fd, (off_t)size) != 0) {
            return errno_result(errno);
        }
        return HOS_RESULT_SUCCESS;
    }
    case 4: /* GetSize -> s64 */ {
        struct stat st;
        if (fstat(f->fd, &st) != 0) {
            return errno_result(errno);
        }
        int64_t size = st.st_size;
        hos_ipc_out_write(ctx, &size, 8);
        return HOS_RESULT_SUCCESS;
    }
    case 5: /* OperateRange -> QueryRangeInfo (0x20) */ {
        uint8_t info[0x20] = {0};
        hos_ipc_out_write(ctx, info, sizeof(info));
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static void file_destroy(hos_object *obj) {
    fs_file *f = obj->ctx;
    if (f->fd >= 0) {
        close(f->fd);
    }
    f->used = 0;
}

static const hos_service_class g_file_class = {
    .name = "fs:IFile",
    .dispatch = file_dispatch,
    .destroy = file_destroy,
};

/* ---- IDirectory ------------------------------------------------------------------- */

static int dir_entry_wanted(const fs_dir *d, const char *name, uint8_t *type, int64_t *size) {
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }
    char full[PATH_MAX];
    if (snprintf(full, sizeof(full), "%s/%s", d->path, name) >= (int)sizeof(full)) {
        return 0;
    }
    struct stat st;
    if (stat(full, &st) != 0) {
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        *type = ENTRY_DIRECTORY;
        *size = 0;
        return (d->mode & DIR_DIRECTORIES) != 0;
    }
    *type = ENTRY_FILE;
    *size = st.st_size;
    return (d->mode & DIR_FILES) != 0;
}

static hos_result dir_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    fs_dir *d = obj->ctx;
    switch (ctx->cmd) {
    case 0: /* Read(out buffer of DirectoryEntry) -> s64 count */ {
        if (ctx->num_b == 0) {
            return FS_INVALID_ARGUMENT;
        }
        uint8_t *out = (uint8_t *)ctx->b[0].addr;
        uint64_t cap = ctx->b[0].size / FS_ENTRY_SIZE;
        int64_t count = 0;
        struct dirent *e;
        while ((uint64_t)count < cap && (e = readdir(d->dir)) != NULL) {
            uint8_t type;
            int64_t size;
            if (!dir_entry_wanted(d, e->d_name, &type, &size)) {
                continue;
            }
            uint8_t *ent = out + count * FS_ENTRY_SIZE;
            memset(ent, 0, FS_ENTRY_SIZE);
            strncpy((char *)ent, e->d_name, FS_PATH_MAX - 1);
            ent[0x304] = type;
            memcpy(ent + 0x308, &size, 8);
            count++;
        }
        hos_ipc_out_write(ctx, &count, 8);
        return HOS_RESULT_SUCCESS;
    }
    case 1: /* GetEntryCount -> s64 */ {
        long pos = telldir(d->dir);
        rewinddir(d->dir);
        int64_t count = 0;
        struct dirent *e;
        while ((e = readdir(d->dir)) != NULL) {
            uint8_t type;
            int64_t size;
            if (dir_entry_wanted(d, e->d_name, &type, &size)) {
                count++;
            }
        }
        seekdir(d->dir, pos);
        hos_ipc_out_write(ctx, &count, 8);
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static void dir_destroy(hos_object *obj) {
    fs_dir *d = obj->ctx;
    if (d->dir) {
        closedir(d->dir);
    }
    d->used = 0;
}

static const hos_service_class g_dir_class = {
    .name = "fs:IDirectory",
    .dispatch = dir_dispatch,
    .destroy = dir_destroy,
};

/* ---- IFileSystem ------------------------------------------------------------------- */

static int remove_tree(const char *path, int keep_root) {
    DIR *d = opendir(path);
    if (!d) {
        return -1;
    }
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        char full[PATH_MAX];
        if (snprintf(full, sizeof(full), "%s/%s", path, e->d_name) >= (int)sizeof(full)) {
            rc = -1;
            continue;
        }
        struct stat st;
        if (lstat(full, &st) != 0) {
            rc = -1;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (remove_tree(full, 0) != 0) {
                rc = -1;
            }
        } else if (unlink(full) != 0) {
            rc = -1;
        }
    }
    closedir(d);
    if (!keep_root && rmdir(path) != 0) {
        rc = -1;
    }
    return rc;
}

static hos_result filesystem_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    fs_mount *m = obj->ctx;
    char path[PATH_MAX];
    hos_result r;

    switch (ctx->cmd) {
    case 0: /* CreateFile(u32 option, s64 size, path) */ {
        struct {
            uint32_t option;
            uint32_t pad;
            int64_t size;
        } a;
        if (!hos_ipc_in_read(ctx, 0, &a, sizeof(a))) {
            return FS_INVALID_ARGUMENT;
        }
        if ((r = resolve(m, ctx, path, sizeof(path))) != 0) {
            return r;
        }
        if (m->read_only) {
            return FS_WRITE_NOT_PERMITTED;
        }
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            return errno_result(errno);
        }
        if (a.size > 0 && ftruncate(fd, (off_t)a.size) != 0) {
            close(fd);
            return errno_result(errno);
        }
        close(fd);
        return HOS_RESULT_SUCCESS;
    }
    case 1: /* DeleteFile(path) */
        if ((r = resolve(m, ctx, path, sizeof(path))) != 0) {
            return r;
        }
        return unlink(path) == 0 ? HOS_RESULT_SUCCESS : errno_result(errno);
    case 2: /* CreateDirectory(path) */
        if ((r = resolve(m, ctx, path, sizeof(path))) != 0) {
            return r;
        }
        return mkdir(path, 0755) == 0 ? HOS_RESULT_SUCCESS : errno_result(errno);
    case 3: /* DeleteDirectory(path) */
        if ((r = resolve(m, ctx, path, sizeof(path))) != 0) {
            return r;
        }
        return rmdir(path) == 0 ? HOS_RESULT_SUCCESS : errno_result(errno);
    case 4:  /* DeleteDirectoryRecursively(path) */
    case 13: /* CleanDirectoryRecursively(path) */
        if ((r = resolve(m, ctx, path, sizeof(path))) != 0) {
            return r;
        }
        return remove_tree(path, ctx->cmd == 13) == 0 ? HOS_RESULT_SUCCESS : errno_result(errno);
    case 5: /* RenameFile(old, new) */
    case 6: /* RenameDirectory(old, new) */ {
        char to[PATH_MAX];
        if ((r = resolve(m, ctx, path, sizeof(path))) != 0 ||
            (r = resolve_second(m, ctx, to, sizeof(to))) != 0) {
            return r;
        }
        return rename(path, to) == 0 ? HOS_RESULT_SUCCESS : errno_result(errno);
    }
    case 7: /* GetEntryType(path) -> u32 */ {
        if ((r = resolve(m, ctx, path, sizeof(path))) != 0) {
            return r;
        }
        struct stat st;
        if (stat(path, &st) != 0) {
            return errno_result(errno);
        }
        uint32_t type = S_ISDIR(st.st_mode) ? ENTRY_DIRECTORY : ENTRY_FILE;
        hos_ipc_out_write(ctx, &type, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 8: /* OpenFile(u32 mode, path) -> IFile */ {
        uint32_t mode = 0;
        hos_ipc_in_read(ctx, 0, &mode, 4);
        if ((r = resolve(m, ctx, path, sizeof(path))) != 0) {
            return r;
        }
        if ((mode & (OPEN_WRITE | OPEN_APPEND)) && m->read_only) {
            return FS_WRITE_NOT_PERMITTED;
        }
        int flags = (mode & OPEN_WRITE) ? O_RDWR : O_RDONLY;
        if (mode & OPEN_APPEND) {
            flags = O_RDWR;
        }
        int fd = open(path, flags);
        if (fd < 0) {
            return errno_result(errno);
        }
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
            close(fd);
            return FS_PATH_NOT_FOUND;
        }
        fs_file *f;
        POOL_TAKE(g_files, POOL_FILES, f);
        if (!f) {
            close(fd);
            return HOS_KERNEL_OUT_OF_RESOURCE;
        }
        f->fd = fd;
        f->mode = mode | ((mode & OPEN_APPEND) ? OPEN_WRITE : 0);
        hos_object *o = hos_ipc_object_new(&g_file_class, f);
        if (!o) {
            file_destroy(&(hos_object){.ctx = f});
            return HOS_KERNEL_OUT_OF_RESOURCE;
        }
        return hos_ipc_return_object(ctx, o);
    }
    case 9: /* OpenDirectory(u32 mode, path) -> IDirectory */ {
        uint32_t mode = 0;
        hos_ipc_in_read(ctx, 0, &mode, 4);
        if ((r = resolve(m, ctx, path, sizeof(path))) != 0) {
            return r;
        }
        DIR *dir = opendir(path);
        if (!dir) {
            return errno_result(errno);
        }
        fs_dir *d;
        POOL_TAKE(g_dirs, POOL_DIRS, d);
        if (!d) {
            closedir(dir);
            return HOS_KERNEL_OUT_OF_RESOURCE;
        }
        d->dir = dir;
        d->mode = mode ? mode : (DIR_DIRECTORIES | DIR_FILES);
        memcpy(d->path, path, strlen(path) + 1); /* resolve() bounded it to PATH_MAX */
        hos_object *o = hos_ipc_object_new(&g_dir_class, d);
        if (!o) {
            dir_destroy(&(hos_object){.ctx = d});
            return HOS_KERNEL_OUT_OF_RESOURCE;
        }
        return hos_ipc_return_object(ctx, o);
    }
    case 10: /* Commit */
        return HOS_RESULT_SUCCESS;
    case 11: /* GetFreeSpaceSize(path) -> s64 */
    case 12: /* GetTotalSpaceSize(path) -> s64 */ {
        struct statvfs vs;
        if (statvfs(m->root, &vs) != 0) {
            return errno_result(errno);
        }
        int64_t v = (int64_t)vs.f_frsize * (int64_t)(ctx->cmd == 11 ? vs.f_bavail : vs.f_blocks);
        hos_ipc_out_write(ctx, &v, 8);
        return HOS_RESULT_SUCCESS;
    }
    case 14: /* GetFileTimeStampRaw(path) -> 0x20 */ {
        if ((r = resolve(m, ctx, path, sizeof(path))) != 0) {
            return r;
        }
        struct stat st;
        if (stat(path, &st) != 0) {
            return errno_result(errno);
        }
        struct {
            int64_t created, modified, accessed;
            uint8_t is_valid;
            uint8_t pad[7];
        } ts = {st.st_ctime, st.st_mtime, st.st_atime, 1, {0}};
        hos_ipc_out_write(ctx, &ts, sizeof(ts));
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static void filesystem_destroy(hos_object *obj) {
    fs_mount *m = obj->ctx;
    m->used = 0;
}

static const hos_service_class g_filesystem_class = {
    .name = "fs:IFileSystem",
    .dispatch = filesystem_dispatch,
    .destroy = filesystem_destroy,
};

static hos_result open_mount(hos_ipc_ctx *ctx, const char *env, int read_only, hos_result missing) {
    const char *root = getenv(env);
    if (!root || !*root) {
        HOS_LOGW(MOD, "%s is not set; mount unavailable", env);
        return missing;
    }
    struct stat st;
    if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        HOS_LOGW(MOD, "%s=%s is not a directory", env, root);
        return missing;
    }
    fs_mount *m;
    POOL_TAKE(g_mounts, POOL_FS, m);
    if (!m) {
        return HOS_KERNEL_OUT_OF_RESOURCE;
    }
    m->read_only = read_only;
    strncpy(m->root, root, sizeof(m->root) - 1);
    size_t len = strlen(m->root);
    while (len > 1 && m->root[len - 1] == '/') {
        m->root[--len] = '\0';
    }
    hos_object *o = hos_ipc_object_new(&g_filesystem_class, m);
    if (!o) {
        m->used = 0;
        return HOS_KERNEL_OUT_OF_RESOURCE;
    }
    HOS_LOGD(MOD, "mounted %s at %s", env, m->root);
    return hos_ipc_return_object(ctx, o);
}

/* ---- IStorage ----------------------------------------------------------------------- */

static hos_result storage_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    fs_storage *s = obj->ctx;
    switch (ctx->cmd) {
    case 0: /* Read(s64 offset, s64 size, out buffer) */ {
        int64_t offset = 0, size = 0;
        if (!hos_ipc_in_read(ctx, 0, &offset, 8) || !hos_ipc_in_read(ctx, 8, &size, 8) ||
            ctx->num_b == 0) {
            return FS_INVALID_ARGUMENT;
        }
        if (offset < 0 || size < 0) {
            return FS_OUT_OF_RANGE;
        }
        uint64_t n = (uint64_t)size < ctx->b[0].size ? (uint64_t)size : ctx->b[0].size;
        uint8_t *p = (uint8_t *)ctx->b[0].addr;
        if (s->img) {
            return romfs_image_read(s->img, (uint64_t)offset, p, n);
        }
        while (n) {
            ssize_t got = pread(s->fd, p, n, (off_t)offset);
            if (got < 0) {
                return errno_result(errno);
            }
            if (got == 0) {
                memset(p, 0, n); /* reads past the end return zeros, as on HOS */
                break;
            }
            p += got;
            offset += got;
            n -= (uint64_t)got;
        }
        return HOS_RESULT_SUCCESS;
    }
    case 1: /* Write */
        return s->read_only ? FS_WRITE_NOT_PERMITTED : FS_UNSUPPORTED_OPERATION;
    case 2: /* Flush */
        return HOS_RESULT_SUCCESS;
    case 3: /* SetSize */
        return FS_UNSUPPORTED_OPERATION;
    case 4: /* GetSize -> s64 */ {
        int64_t size;
        if (s->img) {
            size = (int64_t)romfs_image_size(s->img);
        } else {
            struct stat st;
            if (fstat(s->fd, &st) != 0) {
                return errno_result(errno);
            }
            size = st.st_size;
        }
        hos_ipc_out_write(ctx, &size, 8);
        return HOS_RESULT_SUCCESS;
    }
    case 5: /* OperateRange */ {
        uint8_t info[0x20] = {0};
        hos_ipc_out_write(ctx, info, sizeof(info));
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static void storage_destroy(hos_object *obj) {
    fs_storage *s = obj->ctx;
    if (s->fd >= 0) {
        close(s->fd);
    }
    s->used = 0;
}

static const hos_service_class g_storage_class = {
    .name = "fs:IStorage",
    .dispatch = storage_dispatch,
    .destroy = storage_destroy,
};

static hos_result open_romfs_storage(hos_ipc_ctx *ctx) {
    const char *path = getenv("HOS_ROMFS");
    if (!path || !*path) {
        HOS_LOGW(MOD, "HOS_ROMFS is not set; no data storage");
        return FS_TARGET_NOT_FOUND;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        HOS_LOGW(MOD, "HOS_ROMFS=%s: %s", path, strerror(errno));
        return FS_TARGET_NOT_FOUND;
    }
    int fd = -1;
    if (S_ISDIR(st.st_mode)) {
        /* A directory: build the image once and share it between sessions. */
        pthread_mutex_lock(&g_lock);
        if (!g_romfs_dir_image) {
            hos_result r = romfs_build_from_dir(path, &g_romfs_dir_image);
            if (HOS_FAILED(r)) {
                pthread_mutex_unlock(&g_lock);
                HOS_LOGW(MOD, "HOS_ROMFS=%s: cannot build a romfs image", path);
                return FS_TARGET_NOT_FOUND;
            }
        }
        pthread_mutex_unlock(&g_lock);
    } else {
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            HOS_LOGW(MOD, "HOS_ROMFS=%s: %s", path, strerror(errno));
            return FS_TARGET_NOT_FOUND;
        }
    }
    fs_storage *s;
    POOL_TAKE(g_storages, POOL_STORAGES, s);
    if (!s) {
        if (fd >= 0) {
            close(fd);
        }
        return HOS_KERNEL_OUT_OF_RESOURCE;
    }
    s->fd = fd;
    s->img = (fd < 0) ? g_romfs_dir_image : NULL;
    s->read_only = 1;
    hos_object *o = hos_ipc_object_new(&g_storage_class, s);
    if (!o) {
        storage_destroy(&(hos_object){.ctx = s});
        return HOS_KERNEL_OUT_OF_RESOURCE;
    }
    HOS_LOGD(MOD, "data storage: %s", path);
    return hos_ipc_return_object(ctx, o);
}

/* ---- IDeviceOperator, IEventNotifier ---------------------------------------------------- */

static hos_result device_operator_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 0: /* IsSdCardInserted */ {
        const char *sd = getenv("HOS_SDMC");
        uint8_t v = (sd && *sd) ? 1 : 0;
        hos_ipc_out_write(ctx, &v, 1);
        return HOS_RESULT_SUCCESS;
    }
    case 200: /* IsGameCardInserted */ {
        uint8_t v = 0;
        hos_ipc_out_write(ctx, &v, 1);
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

static const hos_service_class g_device_operator_class = {
    .name = "fs:IDeviceOperator",
    .dispatch = device_operator_dispatch,
};

static hos_result event_notifier_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    if (ctx->cmd != 0) { /* GetEventHandle */
        return hos_ipc_unimplemented(obj, ctx);
    }
    void *ev;
    uint32_t h = hos_ipc_event_create(&ev);
    if (!h) {
        return HOS_KERNEL_OUT_OF_RESOURCE;
    }
    ctx->copy_handles[ctx->num_copy++] = h;
    return HOS_RESULT_SUCCESS;
}

static const hos_service_class g_event_notifier_class = {
    .name = "fs:IEventNotifier",
    .dispatch = event_notifier_dispatch,
};

/* ---- fsp-srv ---------------------------------------------------------------------------- */

static hos_result fsp_dispatch(hos_object *obj, hos_ipc_ctx *ctx) {
    switch (ctx->cmd) {
    case 1: /* SetCurrentProcess */
        return HOS_RESULT_SUCCESS;
    case 18: /* OpenSdCardFileSystem */
        return open_mount(ctx, "HOS_SDMC", 0, FS_SD_CARD_NOT_PRESENT);
    case 51: /* OpenSaveDataFileSystem(u8 space id, SaveDataAttribute) */
    case 52: /* OpenSaveDataFileSystemBySystemSaveDataId */
    case 53: /* OpenReadOnlySaveDataFileSystem */
        return open_mount(ctx, "HOS_SAVE", ctx->cmd == 53, FS_TARGET_NOT_FOUND);
    case 22:   /* CreateSaveDataFileSystem */
    case 23:   /* CreateSaveDataFileSystemBySystemSaveDataId */
    case 620:  /* SetSdCardEncryptionSeed */
    case 1000: /* SetBisRootForHost */
    case 1001: /* SetSaveDataSize */
    case 1002: /* SetSaveDataRootPath */
    case 1003: /* DisableAutoSaveDataCreation */
    case 1004: /* SetGlobalAccessLogMode */
    case 1006: /* OutputAccessLogToSdCard */
        return HOS_RESULT_SUCCESS;
    case 200: /* OpenDataStorageByCurrentProcess */
    case 201: /* OpenDataStorageByProgramId */
    case 202: /* OpenDataStorageByDataId */
    case 203: /* OpenPatchDataStorageByCurrentProcess */
        return open_romfs_storage(ctx);
    case 400: /* OpenDeviceOperator */
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_device_operator_class, NULL));
    case 500: /* OpenSdCardDetectionEventNotifier */
    case 501: /* OpenGameCardDetectionEventNotifier */
        return hos_ipc_return_object(ctx, hos_ipc_object_new(&g_event_notifier_class, NULL));
    case 1005: /* GetGlobalAccessLogMode -> u32 */ {
        uint32_t v = 0;
        hos_ipc_out_write(ctx, &v, 4);
        return HOS_RESULT_SUCCESS;
    }
    case 1011: /* GetProgramIndexForAccessLog -> u32 index, u32 count */ {
        uint32_t v[2] = {0, 1};
        hos_ipc_out_write(ctx, v, 8);
        return HOS_RESULT_SUCCESS;
    }
    default:
        return hos_ipc_unimplemented(obj, ctx);
    }
}

const hos_service_class hos_fsp_class = {
    .name = "fsp-srv",
    .dispatch = fsp_dispatch,
    .destroy = NULL,
};
