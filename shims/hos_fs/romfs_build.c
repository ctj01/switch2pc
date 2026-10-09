#include "romfs_build.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/hos_log.h"

#define MOD "romfs"

#define ROMFS_HEADER_SIZE 0x50
#define ROMFS_INVALID     0xFFFFFFFFu
#define ROMFS_DATA_ALIGN  0x10
#define ARENA_BYTES       (64u << 20) /* virtual; pages are touched on use */
#define MAX_DEPTH         256

typedef struct dir_entry {
    uint32_t parent;  /* index into dirs */
    uint32_t sibling; /* next dir in the same parent, or ROMFS_INVALID */
    uint32_t child;   /* first child dir */
    uint32_t file;    /* first file */
    uint32_t name_off;
    uint32_t name_len;
    uint32_t table_off; /* byte offset in the dir table */
} dir_entry;

typedef struct file_entry {
    uint32_t parent;
    uint32_t sibling;
    uint32_t name_off;
    uint32_t name_len;
    uint32_t table_off;
    uint64_t data_off;
    uint64_t size;
} file_entry;

struct romfs_image {
    char root[PATH_MAX];
    dir_entry *dirs;
    file_entry *files;
    char *names;
    uint32_t num_dirs, num_files;
    uint32_t names_used;
    uint8_t *meta; /* header + tables */
    uint64_t meta_size;
    uint64_t data_off; /* image offset of file data == meta_size */
    uint64_t data_size;
};

static void *arena(size_t bytes) {
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                   -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static uint32_t align4(uint32_t v) {
    return (v + 3) & ~3u;
}

static uint32_t romfs_hash(uint32_t parent_off, const char *name, uint32_t len) {
    uint32_t h = parent_off ^ 123456789u;
    for (uint32_t i = 0; i < len; i++) {
        h = (h >> 5) | (h << 27);
        h ^= (uint8_t)name[i];
    }
    return h;
}

static int is_prime(uint32_t n) {
    if (n < 2) {
        return 0;
    }
    for (uint32_t i = 2; i * i <= n; i++) {
        if (n % i == 0) {
            return 0;
        }
    }
    return 1;
}

static uint32_t hash_table_count(uint32_t entries) {
    if (entries < 3) {
        return 3;
    }
    if (entries < 19) {
        return entries | 1;
    }
    uint32_t n = entries | 1;
    while (!is_prime(n)) {
        n += 2;
    }
    return n;
}

/* Host path of directory `d`: root plus the names up the parent chain. */
static int dir_host_path(const romfs_image *img, uint32_t d, char *out, size_t cap) {
    uint32_t chain[MAX_DEPTH];
    int depth = 0;
    while (d != 0 && depth < MAX_DEPTH) {
        chain[depth++] = d;
        d = img->dirs[d].parent;
    }
    int n = snprintf(out, cap, "%s", img->root);
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    size_t pos = (size_t)n;
    for (int i = depth - 1; i >= 0; i--) {
        const dir_entry *e = &img->dirs[chain[i]];
        if (pos + 1 + e->name_len + 1 > cap) {
            return 0;
        }
        out[pos++] = '/';
        memcpy(out + pos, img->names + e->name_off, e->name_len);
        pos += e->name_len;
        out[pos] = '\0';
    }
    return 1;
}

static int file_host_path(const romfs_image *img, uint32_t f, char *out, size_t cap) {
    const file_entry *e = &img->files[f];
    if (!dir_host_path(img, e->parent, out, cap)) {
        return 0;
    }
    size_t pos = strlen(out);
    if (pos + 1 + e->name_len + 1 > cap) {
        return 0;
    }
    out[pos++] = '/';
    memcpy(out + pos, img->names + e->name_off, e->name_len);
    out[pos + e->name_len] = '\0';
    return 1;
}

static uint32_t add_name(romfs_image *img, const char *name, uint32_t len) {
    if ((uint64_t)img->names_used + len + 1 > ARENA_BYTES) {
        return ROMFS_INVALID;
    }
    uint32_t off = img->names_used;
    memcpy(img->names + off, name, len);
    img->names[off + len] = '\0';
    img->names_used += len + 1;
    return off;
}

/* Breadth-first walk: every directory is listed once, children appended. */
static hos_result walk(romfs_image *img) {
    uint32_t max_dirs = ARENA_BYTES / sizeof(dir_entry);
    uint32_t max_files = ARENA_BYTES / sizeof(file_entry);
    char path[PATH_MAX];

    for (uint32_t d = 0; d < img->num_dirs; d++) {
        if (!dir_host_path(img, d, path, sizeof(path))) {
            return S2PC_ERR_UNSUPPORTED;
        }
        DIR *dir = opendir(path);
        if (!dir) {
            HOS_LOGW(MOD, "%s: %s", path, strerror(errno));
            continue;
        }
        uint32_t last_dir = ROMFS_INVALID, last_file = ROMFS_INVALID;
        struct dirent *e;
        while ((e = readdir(dir)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                continue;
            }
            uint32_t len = (uint32_t)strlen(e->d_name);
            char full[PATH_MAX];
            if (snprintf(full, sizeof(full), "%s/%s", path, e->d_name) >= (int)sizeof(full)) {
                continue;
            }
            struct stat st;
            if (stat(full, &st) != 0) {
                continue;
            }
            uint32_t name_off = add_name(img, e->d_name, len);
            if (name_off == ROMFS_INVALID) {
                closedir(dir);
                return S2PC_ERR_NO_MEMORY;
            }
            if (S_ISDIR(st.st_mode)) {
                if (img->num_dirs >= max_dirs) {
                    closedir(dir);
                    return S2PC_ERR_NO_MEMORY;
                }
                uint32_t idx = img->num_dirs++;
                dir_entry *n = &img->dirs[idx];
                memset(n, 0, sizeof(*n));
                n->parent = d;
                n->sibling = ROMFS_INVALID;
                n->child = ROMFS_INVALID;
                n->file = ROMFS_INVALID;
                n->name_off = name_off;
                n->name_len = len;
                if (last_dir == ROMFS_INVALID) {
                    img->dirs[d].child = idx;
                } else {
                    img->dirs[last_dir].sibling = idx;
                }
                last_dir = idx;
            } else if (S_ISREG(st.st_mode)) {
                if (img->num_files >= max_files) {
                    closedir(dir);
                    return S2PC_ERR_NO_MEMORY;
                }
                uint32_t idx = img->num_files++;
                file_entry *n = &img->files[idx];
                memset(n, 0, sizeof(*n));
                n->parent = d;
                n->sibling = ROMFS_INVALID;
                n->name_off = name_off;
                n->name_len = len;
                n->size = (uint64_t)st.st_size;
                if (last_file == ROMFS_INVALID) {
                    img->dirs[d].file = idx;
                } else {
                    img->files[last_file].sibling = idx;
                }
                last_file = idx;
            }
        }
        closedir(dir);
    }
    return HOS_RESULT_SUCCESS;
}

static void put32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

static void put64(uint8_t *p, uint64_t v) {
    memcpy(p, &v, 8);
}

static hos_result build_tables(romfs_image *img) {
    /* table offsets for every entry */
    uint32_t dir_table_size = 0;
    for (uint32_t d = 0; d < img->num_dirs; d++) {
        img->dirs[d].table_off = dir_table_size;
        dir_table_size += 24 + align4(img->dirs[d].name_len);
    }
    uint32_t file_table_size = 0;
    uint64_t data = 0;
    for (uint32_t f = 0; f < img->num_files; f++) {
        img->files[f].table_off = file_table_size;
        file_table_size += 32 + align4(img->files[f].name_len);
        img->files[f].data_off = data;
        data =
            (data + img->files[f].size + ROMFS_DATA_ALIGN - 1) & ~(uint64_t)(ROMFS_DATA_ALIGN - 1);
    }
    img->data_size = data;

    uint32_t dir_buckets = hash_table_count(img->num_dirs);
    uint32_t file_buckets = hash_table_count(img->num_files);
    uint64_t dir_hash_off = ROMFS_HEADER_SIZE;
    uint64_t dir_table_off = dir_hash_off + (uint64_t)dir_buckets * 4;
    uint64_t file_hash_off = dir_table_off + dir_table_size;
    uint64_t file_table_off = file_hash_off + (uint64_t)file_buckets * 4;
    uint64_t data_off = (file_table_off + file_table_size + ROMFS_DATA_ALIGN - 1) &
                        ~(uint64_t)(ROMFS_DATA_ALIGN - 1);

    img->meta_size = data_off;
    img->data_off = data_off;
    img->meta = arena((size_t)data_off);
    if (!img->meta) {
        return S2PC_ERR_NO_MEMORY;
    }
    uint8_t *m = img->meta;
    put64(m + 0x00, ROMFS_HEADER_SIZE);
    put64(m + 0x08, dir_hash_off);
    put64(m + 0x10, (uint64_t)dir_buckets * 4);
    put64(m + 0x18, dir_table_off);
    put64(m + 0x20, dir_table_size);
    put64(m + 0x28, file_hash_off);
    put64(m + 0x30, (uint64_t)file_buckets * 4);
    put64(m + 0x38, file_table_off);
    put64(m + 0x40, file_table_size);
    put64(m + 0x48, data_off);

    uint8_t *dir_hash = m + dir_hash_off;
    uint8_t *file_hash = m + file_hash_off;
    memset(dir_hash, 0xFF, (size_t)dir_buckets * 4);
    memset(file_hash, 0xFF, (size_t)file_buckets * 4);

    for (uint32_t d = 0; d < img->num_dirs; d++) {
        const dir_entry *e = &img->dirs[d];
        uint8_t *p = m + dir_table_off + e->table_off;
        uint32_t parent_off = img->dirs[e->parent].table_off;
        put32(p + 0, parent_off);
        put32(p + 4, e->sibling == ROMFS_INVALID ? ROMFS_INVALID : img->dirs[e->sibling].table_off);
        put32(p + 8, e->child == ROMFS_INVALID ? ROMFS_INVALID : img->dirs[e->child].table_off);
        put32(p + 12, e->file == ROMFS_INVALID ? ROMFS_INVALID : img->files[e->file].table_off);
        uint32_t bucket =
            romfs_hash(d == 0 ? 0 : parent_off, img->names + e->name_off, e->name_len) %
            dir_buckets;
        uint32_t head;
        memcpy(&head, dir_hash + bucket * 4, 4);
        put32(p + 16, head);
        put32(dir_hash + bucket * 4, e->table_off);
        put32(p + 20, e->name_len);
        memset(p + 24, 0, align4(e->name_len));
        memcpy(p + 24, img->names + e->name_off, e->name_len);
    }
    for (uint32_t f = 0; f < img->num_files; f++) {
        const file_entry *e = &img->files[f];
        uint8_t *p = m + file_table_off + e->table_off;
        uint32_t parent_off = img->dirs[e->parent].table_off;
        put32(p + 0, parent_off);
        put32(p + 4,
              e->sibling == ROMFS_INVALID ? ROMFS_INVALID : img->files[e->sibling].table_off);
        put64(p + 8, e->data_off);
        put64(p + 16, e->size);
        uint32_t bucket =
            romfs_hash(parent_off, img->names + e->name_off, e->name_len) % file_buckets;
        uint32_t head;
        memcpy(&head, file_hash + bucket * 4, 4);
        put32(p + 24, head);
        put32(file_hash + bucket * 4, e->table_off);
        put32(p + 28, e->name_len);
        memset(p + 32, 0, align4(e->name_len));
        memcpy(p + 32, img->names + e->name_off, e->name_len);
    }
    return HOS_RESULT_SUCCESS;
}

hos_result romfs_build_from_dir(const char *dir, romfs_image **out) {
    *out = NULL;
    romfs_image *img = arena(sizeof(*img));
    if (!img) {
        return S2PC_ERR_NO_MEMORY;
    }
    snprintf(img->root, sizeof(img->root), "%s", dir);
    size_t len = strlen(img->root);
    while (len > 1 && img->root[len - 1] == '/') {
        img->root[--len] = '\0';
    }
    img->dirs = arena(ARENA_BYTES);
    img->files = arena(ARENA_BYTES);
    img->names = arena(ARENA_BYTES);
    if (!img->dirs || !img->files || !img->names) {
        romfs_image_free(img);
        return S2PC_ERR_NO_MEMORY;
    }
    /* root directory: empty name, parent itself */
    memset(&img->dirs[0], 0, sizeof(dir_entry));
    img->dirs[0].sibling = ROMFS_INVALID;
    img->dirs[0].child = ROMFS_INVALID;
    img->dirs[0].file = ROMFS_INVALID;
    img->dirs[0].name_off = add_name(img, "", 0);
    img->num_dirs = 1;

    hos_result r = walk(img);
    if (HOS_OK(r)) {
        r = build_tables(img);
    }
    if (HOS_FAILED(r)) {
        romfs_image_free(img);
        return r;
    }
    HOS_LOGI(MOD, "built romfs from %s: %u directories, %u files, %llu bytes of data", img->root,
             img->num_dirs - 1, img->num_files, (unsigned long long)img->data_size);
    *out = img;
    return HOS_RESULT_SUCCESS;
}

uint64_t romfs_image_size(const romfs_image *img) {
    return img->data_off + img->data_size;
}

/* Index of the first file whose data range ends after `pos`, or num_files. */
static uint32_t first_file_after(const romfs_image *img, uint64_t pos) {
    uint32_t lo = 0, hi = img->num_files;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const file_entry *e = &img->files[mid];
        if (e->data_off + e->size <= pos) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

hos_result romfs_image_read(const romfs_image *img, uint64_t offset, void *dst, uint64_t size) {
    uint8_t *out = dst;
    memset(out, 0, (size_t)size);
    uint64_t end = offset + size;

    if (offset < img->meta_size) {
        uint64_t n = (end < img->meta_size ? end : img->meta_size) - offset;
        memcpy(out, img->meta + offset, (size_t)n);
        out += n;
        offset += n;
    }
    if (offset >= end) {
        return HOS_RESULT_SUCCESS;
    }

    uint64_t data_pos = offset - img->data_off;
    uint64_t data_end = end - img->data_off;
    char path[PATH_MAX];
    for (uint32_t f = first_file_after(img, data_pos); f < img->num_files; f++) {
        const file_entry *e = &img->files[f];
        if (e->data_off >= data_end) {
            break;
        }
        uint64_t start = e->data_off > data_pos ? e->data_off : data_pos;
        uint64_t stop = e->data_off + e->size < data_end ? e->data_off + e->size : data_end;
        if (stop <= start) {
            continue;
        }
        if (!file_host_path(img, f, path, sizeof(path))) {
            continue;
        }
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            HOS_LOGW(MOD, "%s: %s", path, strerror(errno));
            continue;
        }
        uint8_t *p = out + (start - data_pos);
        uint64_t left = stop - start;
        off_t at = (off_t)(start - e->data_off);
        while (left) {
            ssize_t got = pread(fd, p, (size_t)left, at);
            if (got <= 0) {
                break;
            }
            p += got;
            at += got;
            left -= (uint64_t)got;
        }
        close(fd);
    }
    return HOS_RESULT_SUCCESS;
}

void romfs_image_free(romfs_image *img) {
    if (!img) {
        return;
    }
    if (img->dirs) {
        munmap(img->dirs, ARENA_BYTES);
    }
    if (img->files) {
        munmap(img->files, ARENA_BYTES);
    }
    if (img->names) {
        munmap(img->names, ARENA_BYTES);
    }
    if (img->meta) {
        munmap(img->meta, (size_t)img->meta_size);
    }
    munmap(img, sizeof(*img));
}
